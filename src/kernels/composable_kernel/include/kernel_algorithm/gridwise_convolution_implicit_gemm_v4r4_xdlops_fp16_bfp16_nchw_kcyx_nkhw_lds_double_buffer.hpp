#ifndef CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_FP16_BFP16_NCHW_KCYX_NKHW_HPP_LDS_DOUBLE_BUFFER_HPP
#define CK_GRIDWISE_CONVOLUTION_IMPLICIT_GEMM_V4R4_FP16_BFP16_NCHW_KCYX_NKHW_HPP_LDS_DOUBLE_BUFFER_HPP

#include "common_header.hpp"
#include "ConstantTensorDescriptor.hpp"
#include "ConstantMergedTensorDescriptor.hpp"
#include "ConstantMatrixDescriptor.hpp"
#include "blockwise_generic_tensor_slice_copy.hpp"
#include "blockwise_gemm_xdlops.hpp"
#include "threadwise_generic_tensor_slice_copy.hpp"

namespace ck {

// B = merge(N, Ho, Wo)
template <index_t GridSize,
          index_t BlockSize,
          class Float,
          class AccDataType,
          class InGlobalDesc,
          class WeiGlobalDesc,
          class OutGlobalDesc,
          class ConvStrides,
          class ConvDilations,
          index_t BPerBlock,
          index_t KPerBlock,
          index_t EPerBlock,
          index_t EPack,
          index_t GemmMPerWave,
          index_t GemmNPerWave,
          index_t GemmMWaves,
          index_t GemmNWaves,
          index_t GemmDataPerReadA,
          index_t GemmDataPerReadB,
          bool EnableXdlops,
          class InBlockCopySubLengths_E_B,
          class InBlockCopyClusterLengths_E_B,
          class InBlockCopyThreadClusterArrangeOrder,
          class InBlockCopySrcAccessOrder,
          class InBlockCopyDstAccessOrder,
          index_t InBlockCopyDataPerAccess_B,
          class WeiBlockCopySubLengths_E_K,
          class WeiBlockCopyClusterLengths_E_K,
          class WeiBlockCopyThreadClusterArrangeOrder,
          class WeiBlockCopySrcAccessOrder,
          class WeiBlockCopyDstAccessOrder,
          index_t WeiBlockCopySrcDataPerRead_E,
          index_t WeiBlockCopyDstDataPerWrite_K,
          index_t OutThreadCopyDataPerAccess_B>
struct GridwiseConvolutionImplicitGemm_v4r4_xdlops_fp16_bfp16_nchw_kcyx_nkhw_lds_double_buffer
{
    __device__ void Run(const Float* const __restrict__ p_in_global,
                        const Float* const __restrict__ p_wei_global,
                        Float* const __restrict__ p_out_global) const
    {
        constexpr auto I1 = Number<1>{};
        constexpr auto I2 = Number<2>{};
        constexpr auto I3 = Number<3>{};
        constexpr auto I4 = Number<4>{};

        constexpr auto True = integral_constant<bool, true>{};

        constexpr auto in_n_c_h_w_global_desc  = InGlobalDesc{};
        constexpr auto wei_k_c_y_x_global_desc = WeiGlobalDesc{};
        constexpr auto out_n_k_h_w_global_desc = OutGlobalDesc{};

        constexpr index_t N = in_n_c_h_w_global_desc.GetLengths()[0];
        constexpr index_t C = in_n_c_h_w_global_desc.GetLengths()[1];

        constexpr index_t K  = out_n_k_h_w_global_desc.GetLengths()[1];
        constexpr index_t Ho = out_n_k_h_w_global_desc.GetLengths()[2];
        constexpr index_t Wo = out_n_k_h_w_global_desc.GetLengths()[3];

        constexpr index_t Y = wei_k_c_y_x_global_desc.GetLengths()[2];
        constexpr index_t X = wei_k_c_y_x_global_desc.GetLengths()[3];

        constexpr index_t ConvStrideH = ConvStrides{}[0];
        constexpr index_t ConvStrideW = ConvStrides{}[1];

        constexpr index_t ConvDilationH = ConvDilations{}[0];
        constexpr index_t ConvDilationW = ConvDilations{}[1];

        constexpr index_t nonVectorizedC = C / EPack;
        constexpr index_t E              = nonVectorizedC * Y * X;
        constexpr index_t B              = N * Ho * Wo;

        static_assert((X == 1 || ConvDilationW % InBlockCopyDataPerAccess_B == 0),
                      "wrong! aligment requirement for vectorized global load of input tensor will "
                      "be violated");

        // divide block work by [K, B]
        static_assert(K % KPerBlock == 0 && B % BPerBlock == 0 && E % (2 * EPerBlock) == 0,
                      "wrong! cannot divide work evenly among block");

        constexpr index_t KBlockWork = K / KPerBlock;
        constexpr index_t BBlockWork = B / BPerBlock;

        constexpr auto block_work_desc =
            make_ConstantTensorDescriptor_packed(Sequence<KBlockWork, BBlockWork>{});

        const auto block_work_multi_id =
            block_work_desc.GetMultiIndexFrom1dIndex(get_block_1d_id());

        const index_t k_block_data_on_global = block_work_multi_id[0] * KPerBlock;
        const index_t b_block_data_on_global = block_work_multi_id[1] * BPerBlock;

        // input tensor
        //     tensor descriptor in device memory [N, Ho, Wo, {2C/4C}]
        constexpr auto in_n_ho_wo_2cor4c_global_desc =
            in_n_c_h_w_global_desc.StridedSlice(I2, Number<Ho>{}, Number<ConvStrideH>{})
                .StridedSlice(I3, Number<Wo>{}, Number<ConvStrideW>{})
                .Fold(I1, Number<nonVectorizedC>{})
                .Extract(Sequence<0, 1, 3, 4>{})
                .ReorderGivenNew2Old(Sequence<0, 2, 3, 1>{});

        //     batch descritpor for device memory
        constexpr auto in_c_y_x_global_desc =
            in_n_c_h_w_global_desc.StridedSlice(I2, Number<Y>{}, Number<ConvDilationH>{})
                .StridedSlice(I3, Number<X>{}, Number<ConvDilationW>{})
                .Fold(I1, Number<nonVectorizedC>{})
                .Extract(Sequence<2, 3, 4>{});

        //     merged tensor descriptor in device memory [E, B], src of blockwise copy
        constexpr auto in_e_b_global_desc = make_ConstantMergedTensorDescriptor(
            in_c_y_x_global_desc.Embed(in_n_ho_wo_2cor4c_global_desc),
            Sequence<0, 1, 2>{},
            Sequence<3, 4, 5>{},
            Sequence<6>{});

        //     memory layout descriptor in LDS [E, B, 2Cor4C], dst of blockwise copy
        //     be careful of LDS alignment
        constexpr auto in_e_b_block_desc = make_ConstantTensorDescriptor_aligned(
            Sequence<EPerBlock, BPerBlock, EPack>{},
            Number<math::lcm(InBlockCopyDataPerAccess_B, GemmDataPerReadB, EPack)>{});

        // input blockwise copy
        //     slice a merged tensor, reorder and copy to a normal tensor
        //     this copy operator already has blockwise offset built-in
        auto blockwise_in_copy = BlockwiseGenericTensorSliceCopy_v2<
            BlockSize,
            decltype(in_e_b_global_desc),
            decltype(in_e_b_block_desc),
            MergedTensorCoordinate<decltype(in_e_b_global_desc)>,
            NormalTensorCoordinate<decltype(in_e_b_block_desc)>,
            decltype(in_e_b_block_desc.GetLengths()),
            InBlockCopySubLengths_E_B,
            InBlockCopyClusterLengths_E_B,
            InBlockCopyThreadClusterArrangeOrder,
            InBlockCopySrcAccessOrder,
            InBlockCopyDstAccessOrder,
            1,                           // Src dim to be read in vector form (B dimension)
            2,                           // Dst dim to be written in vector form (EPack dimension)
            InBlockCopyDataPerAccess_B,  // Src dim vector len
            InBlockCopyDataPerAccess_B>( // Dst dim vector len
            {0, b_block_data_on_global, 0},
            {0, 0, 0});

        // weight tensor
        //     tensor descriptor in device memory, src of blockwise copy
        constexpr auto wei_e_k_global_desc =
            wei_k_c_y_x_global_desc.Fold(I1, Number<nonVectorizedC>{})
                .Unfold(I2, I4)
                .ReorderGivenNew2Old(Sequence<2, 0, 1>{});

        //     tensor descriptor in LDS, dst of blockwise copy
        //     be careful of LDS alignment
        constexpr auto wei_e_k_block_desc = make_ConstantTensorDescriptor_aligned(
            Sequence<EPerBlock, KPerBlock, EPack>{},
            Number<math::lcm(WeiBlockCopyDstDataPerWrite_K, GemmDataPerReadA, EPack)>{});

        // operator for blockwise copy of weight into LDS
        //     slice a tensor, and copy it into another tensor
        //     this copy operator already have blockwise offset built-in
        auto blockwise_wei_copy = BlockwiseGenericTensorSliceCopy_v2<
            BlockSize,
            decltype(wei_e_k_global_desc),
            decltype(wei_e_k_block_desc),
            NormalTensorCoordinate<decltype(wei_e_k_global_desc)>,
            NormalTensorCoordinate<decltype(wei_e_k_block_desc)>,
            decltype(wei_e_k_block_desc.GetLengths()),
            WeiBlockCopySubLengths_E_K,
            WeiBlockCopyClusterLengths_E_K,
            WeiBlockCopyThreadClusterArrangeOrder,
            WeiBlockCopySrcAccessOrder,
            WeiBlockCopyDstAccessOrder,
            0,                            // Src dim to be read in vector form (E dimension)
            2,                            // Dst dim to be written in vector form (EPack dimension)
            WeiBlockCopySrcDataPerRead_E, // Src dim vector len
            WeiBlockCopyDstDataPerWrite_K>( // Dst dim vector len
            {0, k_block_data_on_global, 0},
            {0, 0, 0});

        // GEMM definition
        constexpr auto a_e_k_block_mtx_desc =
            make_ConstantMatrixDescriptor_packed(Number<EPerBlock>{}, Number<KPerBlock>{});

        constexpr auto b_e_b_block_mtx_desc =
            make_ConstantMatrixDescriptor_packed(Number<EPerBlock>{}, Number<BPerBlock>{});

        const auto blockwise_gemm = BlockwiseGemmBlockABlockBThreadCTransANormalBNormalC_xdlops<
            BlockSize,
            decltype(a_e_k_block_mtx_desc),
            decltype(b_e_b_block_mtx_desc),
            decltype(mfma_info<Float>{}),
            EnableXdlops,
            GemmMPerWave,
            GemmNPerWave,
            GemmMWaves,
            GemmNWaves,
            GemmDataPerReadA,
            GemmDataPerReadB>{};

        constexpr auto c_k_thread_mtx_desc = blockwise_gemm.GetThreadMatrixCDescriptor();

        constexpr index_t max_align = math::lcm(InBlockCopyDataPerAccess_B,
                                                WeiBlockCopyDstDataPerWrite_K,
                                                GemmDataPerReadA,
                                                GemmDataPerReadB,
                                                EPack);

        constexpr index_t in_block_space =
            math::integer_least_multiple(in_e_b_block_desc.GetElementSpace(), max_align);

        constexpr index_t wei_block_space =
            math::integer_least_multiple(wei_e_k_block_desc.GetElementSpace(), max_align);

        __shared__ Float p_in_block_double[2 * in_block_space];
        __shared__ Float p_wei_block_double[2 * wei_block_space];

        // register allocation for output
        AccDataType p_out_thread[c_k_thread_mtx_desc.GetElementSpace()];

        // zero out threadwise output
        threadwise_matrix_set_zero(c_k_thread_mtx_desc, p_out_thread);
        static_if<EnableXdlops>{}(
            [&](auto) { gcnasm_accvgpr_zero<c_k_thread_mtx_desc.GetElementSpace()>(); });

        const Float* p_wei_block_on_global = p_wei_global;

        // LDS double buffer: preload data into LDS
        {
            blockwise_in_copy.Run(p_in_global, p_in_block_double);
            blockwise_wei_copy.Run(p_wei_global, p_wei_block_double);
        }

        // LDS double buffer: main body
        for(index_t e_block_data_begin = 0; e_block_data_begin + 2 * EPerBlock < E;
            e_block_data_begin += 2 * EPerBlock)
        {
#pragma unroll
            for(index_t iloop = 0; iloop < 2; ++iloop)
            {
                const bool even_loop = (iloop % 2 == 0);

                Float* p_in_block_now =
                    even_loop ? p_in_block_double : p_in_block_double + in_block_space;
                Float* p_wei_block_now =
                    even_loop ? p_wei_block_double : p_wei_block_double + wei_block_space;

                Float* p_in_block_next =
                    even_loop ? p_in_block_double + in_block_space : p_in_block_double;
                Float* p_wei_block_next =
                    even_loop ? p_wei_block_double + wei_block_space : p_wei_block_double;

                Float p_in_register_buffer[blockwise_in_copy.GetRegisterBufferSize()];
                Float p_wei_register_buffer[blockwise_wei_copy.GetRegisterBufferSize()];

                blockwise_in_copy.MoveSrcSlicingWindow(Sequence<EPerBlock, 0, 0>{}, True);
                p_wei_block_on_global += EPerBlock * wei_e_k_global_desc.GetStrides()[0];

                __syncthreads();

                // LDS doubel buffer: load next data from device mem
                blockwise_in_copy.RunLoadRegisterBuffer(p_in_global, p_in_register_buffer);
                blockwise_wei_copy.RunLoadRegisterBuffer(p_wei_block_on_global,
                                                         p_wei_register_buffer);

                // LDS double buffer: GEMM on current data
                const typename vector_type<Float, EPack>::MemoryType* p_a_block_vec =
                    reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                        p_wei_block_now);
                const typename vector_type<Float, EPack>::MemoryType* p_b_block_vec =
                    reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                        p_in_block_now);
                blockwise_gemm.Run(p_a_block_vec, p_b_block_vec, p_out_thread);

                // LDS double buffer: store next data to LDS
                blockwise_in_copy.RunStoreRegisterBuffer(p_in_register_buffer, p_in_block_next);
                blockwise_wei_copy.RunStoreRegisterBuffer(p_wei_register_buffer, p_wei_block_next);
            }
        }

        // LDS double buffer: tail
        {
            Float p_in_register_buffer[blockwise_in_copy.GetRegisterBufferSize()];
            Float p_wei_register_buffer[blockwise_wei_copy.GetRegisterBufferSize()];

            // even iteration
            blockwise_in_copy.MoveSrcSlicingWindow(Sequence<EPerBlock, 0, 0>{}, True);
            p_wei_block_on_global += EPerBlock * wei_e_k_global_desc.GetStrides()[0];

            __syncthreads();

            // LDS doubel buffer: load next data from device mem
            blockwise_in_copy.RunLoadRegisterBuffer(p_in_global, p_in_register_buffer);
            blockwise_wei_copy.RunLoadRegisterBuffer(p_wei_block_on_global, p_wei_register_buffer);

            // LDS double buffer: GEMM on current data
            // Vectorize the pointer to match with how half/bfloat16 datatypes are
            // processed in gemm operation. Half type packs 4 half values while
            // bfloat16 packs 2 bfloat16 values. Since gemm's matrix A and B
            // 2D indexes are computed with a single value in mind (e.g. float),
            // to retain the same 2D indexes for half/bfloat16, we recast datatype
            // from a single half to 4 packed half/2 packed bfloat16 respectively.
            const typename vector_type<Float, EPack>::MemoryType* p_a_block_vec =
                reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                    p_wei_block_double);
            const typename vector_type<Float, EPack>::MemoryType* p_b_block_vec =
                reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                    p_in_block_double);

            blockwise_gemm.Run(p_a_block_vec, p_b_block_vec, p_out_thread);

            // LDS double buffer: store next data to LDS
            blockwise_in_copy.RunStoreRegisterBuffer(p_in_register_buffer,
                                                     p_in_block_double + in_block_space);
            blockwise_wei_copy.RunStoreRegisterBuffer(p_wei_register_buffer,
                                                      p_wei_block_double + wei_block_space);

            // odd iteration
            __syncthreads();

            p_a_block_vec = reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                p_wei_block_double + wei_block_space);
            p_b_block_vec = reinterpret_cast<const typename vector_type<Float, EPack>::MemoryType*>(
                p_in_block_double + in_block_space);

            // LDS double buffer: GEMM on current data
            blockwise_gemm.Run(p_a_block_vec, p_b_block_vec, p_out_thread);
        }

        // load data from xldop_acc_regs
        static_if<EnableXdlops>{}([&](auto) {
            gcnasm_accvgpr_read<c_k_thread_mtx_desc.GetElementSpace()>(p_out_thread);
        });

        // copy output: register to global memory
        {
            constexpr index_t K2 = blockwise_gemm.OutputLayout.M2;
            constexpr index_t K1 = blockwise_gemm.OutputLayout.M1;
            constexpr index_t K0 = blockwise_gemm.OutputLayout.M0;

            // This is a hack, because slicing a merged dimension is not supported yet.
            //     dst descriptor
            constexpr auto out_k0_k1_k2_b_global_desc = make_ConstantMergedTensorDescriptor(
                out_n_k_h_w_global_desc.Fold(I1, Number<K1>{}, Number<K2>{}),
                Sequence<1>{},
                Sequence<2>{},
                Sequence<3>{},
                Sequence<0, 4, 5>{});

            //     src descriptor
            constexpr auto out_k0_k1_k2_b_thread_desc =
                make_ConstantTensorDescriptor_packed(Sequence<K2, 1, K0, 1>{});

            using OutThreadCopySliceLengths = Sequence<K2, 1, K0, 1>;

            constexpr index_t NumKPerBlk = out_k0_k1_k2_b_thread_desc.GetElementSpace();
            constexpr index_t NumBlks    = GemmMPerWave / NumKPerBlk;

            for(index_t i = 0; i < NumBlks; ++i)
            {
                // calculate origin of thread output tensor on global memory
                //     blockwise GEMM c matrix starting index
                const auto c_thread_mtx_on_block = blockwise_gemm.GetBeginOfThreadMatrixC(i);

                const index_t k_thread_data_on_global =
                    k_block_data_on_global + c_thread_mtx_on_block.row;

                const index_t b_thread_data_on_global =
                    b_block_data_on_global + c_thread_mtx_on_block.col;

                auto threadwise_out_copy = ThreadwiseGenericTensorSliceCopy_v2r1<
                    decltype(out_k0_k1_k2_b_thread_desc),
                    decltype(out_k0_k1_k2_b_global_desc),
                    NormalTensorCoordinate<decltype(out_k0_k1_k2_b_thread_desc)>,
                    MergedTensorCoordinate<decltype(out_k0_k1_k2_b_global_desc)>,
                    OutThreadCopySliceLengths,
                    arithmetic_sequence_gen<0, 4, 1>::type,
                    arithmetic_sequence_gen<0, 4, 1>::type,
                    3, // Src dim to be read in vector form (B dimension)
                    3, // Dst dim to be written in vector form (B dimension)
                    OutThreadCopyDataPerAccess_B,  // Src dim vector len
                    OutThreadCopyDataPerAccess_B>( // Dst dim vector len
                    {0, 0, 0, 0},
                    {k_thread_data_on_global / (K0 * K1),
                     k_thread_data_on_global % (K0 * K1) / K0,
                     k_thread_data_on_global % K0,
                     b_thread_data_on_global});

                threadwise_out_copy.Run(p_out_thread + i * NumKPerBlk, p_out_global);
            }
        }
    }
};

} // namespace ck
#endif
