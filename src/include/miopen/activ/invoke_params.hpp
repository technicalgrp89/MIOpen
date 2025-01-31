/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#pragma once

#include <miopen/invoke_params.hpp>
#include <miopen/tensor.hpp>

namespace miopen {
namespace activ {

struct InvokeParams : public miopen::InvokeParams
{
    InvokeParams() = default;

    double alpha = 0;
    TensorDescriptor x_desc;
    ConstData_t x = nullptr;
    double beta   = 0;
    TensorDescriptor y_desc;
    Data_t y        = nullptr;
    double gamma    = 0;
    size_t x_offset = 0;
    size_t y_offset = 0;
};

struct BwdInvokeParams : public miopen::InvokeParams
{
    BwdInvokeParams() = default;

    double alpha = 0;
    double beta  = 0;
    double gamma = 0;
    TensorDescriptor x_desc;
    TensorDescriptor y_desc;
    TensorDescriptor dx_desc;
    TensorDescriptor dy_desc;
    ConstData_t x    = nullptr;
    ConstData_t y    = nullptr;
    Data_t dx        = nullptr;
    ConstData_t dy   = nullptr;
    size_t x_offset  = 0;
    size_t y_offset  = 0;
    size_t dx_offset = 0;
    size_t dy_offset = 0;
};

} // namespace activ

} // namespace miopen
