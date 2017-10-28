#pragma once
#ifndef __VALLUE_QUANTIZER_H__
#define __VALLUE_QUANTIZER_H__

#include "Basics.h"
#include "BestGpu.h" // for CPUONLY
#ifndef CPUONLY
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <device_launch_parameters.h>
#endif // !CPUONLY

#include <cassert>
#include <stdexcept>

#pragma warning(disable : 4127) // conditional expression is constant

namespace Microsoft { namespace MSR { namespace CNTK {

#ifdef __device__                          // this can be used in CUDA; if this is not defined, then we are compiling in a non-CUDA context
#define cudacode __device__                // CUDA: we assume we ONLY run these functions on CUDA (otherwise we'd need to mess with specifiers of matrixref)
#define cudasharedcode __device__ __host__ // shared on both CUDA and CPU; note that such functions cannot call into __device__ only functions like matrixref::operator(,)
#undef assert
#define assert(c)
#else
#define cudacode // non-CUDA context: defines to nothing
#define cudasharedcode
//#define QUANTUSEPPL
#endif

#ifdef QUANTUSEPPL
#include <ppl.h> // in non-CUDA: also use PPL lib
#endif

template <typename ElemType>
class QuantizedWordHelper;

template <>
class QuantizedWordHelper<float>
{
public:
    typedef unsigned int ValueType;
    typedef int ValueTypeSigned;
    static_assert(sizeof(float) == sizeof(ValueType), "Quantized word size != size of ElemType=float");
};

template <>
class QuantizedWordHelper<double>
{
public:
    typedef unsigned long long ValueType;
    typedef long long ValueTypeSigned;
    static_assert(sizeof(double) == sizeof(ValueType), "Quantized word size != size of ElemType=double");
};

#pragma warning(disable : 4334) // 'operator' : result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)
template <class ElemType>
class ValueQuantizer
{
public:
    typedef typename QuantizedWordHelper<ElemType>::ValueType QWord;
    typedef typename QuantizedWordHelper<ElemType>::ValueType QWordVal;
    typedef typename QuantizedWordHelper<ElemType>::ValueTypeSigned QWordValSigned;
    static const size_t QWordNumBits = 8 * sizeof(QWord);

public:
    cudasharedcode ValueQuantizer(size_t ldNbits, ElemType lower, ElemType upper)
        : ldNbits(ldNbits), Nbits(1 << ldNbits), quantimin(lower), quantimax(upper)
    {
        rangeend = ((QWordVal) 1) << Nbits;

        // post-fix for incorrect shift for no-quant hack (Nbits=32): << arg is taken mod 32!
        // in this case, it's only used as (rangeend-1) which is now correct (before it was 0!)
        if (Nbits >= (8 * sizeof(rangeend)))
        {
            rangeend = 0;
        }

        // must protect against NaN: interval is 0 -> quantization is futile, just emit 0
        if (((quantimax - quantimin) < 1e-36f) || (rangeend == 0))
        {
            qfactor = ufactor = (ElemType) 0.0;
        }
        else
        {
            // make the range asymmetrical, so we get a 0 slot
            size_t usedrangeend = rangeend - (Nbits > 1); // TODO: make this a parameter
            // precompute this for quantize() (see comment there)
            qfactor = usedrangeend / (quantimax - quantimin);
            // and for unquantize()
            ufactor = (quantimax - quantimin) / usedrangeend;
        }

        // set the quantization threshold for the special case of 1-bit
        quantimid = 0.5f * (quantimax + quantimin);
    }

    // log-linearly quantize to a "N-bit float" that only stores the exponent
//#ifndef __CUDACC__ // TODO: figure out how to use the native device functions for the CUDA compilation, and these for the CPU code
    static cudasharedcode int __float_as_int(float f) { return *((int*)&f); }
    static cudasharedcode float __int_as_float(int i) { return *((float*)&i); }
//#endif
    static cudasharedcode int QuantizeLogarithmically(float x, float stddev, int nBits)
    {
        int expRange = (1 << (nBits - 1)); // numeric range of exponent without the sign
        float xn = x / stddev;
        int xi = __float_as_int(xn);
        bool isNeg = x < 0;
        int exp = (xi >> 23) & 0xff;
        int qval = exp + expRange / 2 - 127; // half-way is exponent of 0
        if (qval < 0)
            qval = 0;
        else if (qval >= expRange)
            qval = expRange - 1;
        if (isNeg)
            qval |= expRange;
        return qval;
    }
    static cudasharedcode float UnquantizeLogarithmically(int qval, float stddev, int nBits)
    {
        int expRange = (1 << (nBits - 1)); // numeric range of exponent without the sign
        bool isNeg = qval > 7;
        int exp = qval & (expRange - 1);
        if (exp == 0) // quantize the smallest values to 0
            return 0;
        exp -= expRange / 2 - 127;
        if (isNeg)
            exp |= 256;
        int xi = exp << 23;
        return __int_as_float(xi) * stddev * 1.4f; // 1.4f is an empirical correction factor, which I was too lazy to figure out
    }

    // quantize one value
    // TODO: we can optimize for 1 bit here - very simply use a template arg 'isonebit'
    cudasharedcode float RecoverStdDev() const { return (float)(quantimax - quantimin) / 2.0f/*one-sided*/ / 4.0f/*they are 4*stddev*/; }
    template <bool ZeroThresholdFor1Bit>
    cudasharedcode QWordVal Quantize(ElemType u) const
    {
#if 1
        if (Nbits == 4)
        {
            return (QWordVal)QuantizeLogarithmically((float)u, RecoverStdDev(), (int)Nbits);
        }
        else
#endif
        if (Nbits == QWordNumBits)
        {
            return QuantizeToFullQWord(u);
        }
        // TODO: we may need to optimize this by a template arg
        else if (ldNbits == 0)
        {
            return Quantize1<ZeroThresholdFor1Bit>(u) ? 1 : 0;
        }
        else
        {
            if (u <= quantimin)
            {
                return 0;
            }
            else if (u >= quantimax)
            {
                return (rangeend - 1);
            }
            else
            {
                return (QWordVal)((QWordValSigned)((u - quantimin) * qfactor));
            }
        }
    }

    // unquantize one value
    cudasharedcode ElemType Unquantize(QWordVal u) const
    {
#if 1
        if (Nbits == 4)
        {
            return (ElemType)UnquantizeLogarithmically((int)u, RecoverStdDev(), (int)Nbits);
        }
        else
#endif
        // special branch that does not quantize at all, for testing
        if (Nbits == QWordNumBits)
        {
            return *(ElemType*) &u;
        }

        // Note: in 1-bit case, we want 0.5 -> mean0, 1.5 -> mean1
        return ((u + (ElemType) 0.5) * ufactor) + quantimin;
    }

    // quantize one value --special version for 1 bit
    template <bool ZeroThresholdFor1Bit>
    cudasharedcode bool Quantize1(ElemType u) const
    {
        assert(Nbits == 1);
        if (!ZeroThresholdFor1Bit)
        {
            return u >= quantimid;
        }
        else
        {
            return u >= (ElemType) 0.0;
        }
    }

    // unquantize one value  --special case for 1 bit
    static cudasharedcode ElemType Unquantize1(bool u, ElemType val0, ElemType val1)
    {
        return u ? val1 : val0;
    }

    // how many bits we are quanatizing to
    cudasharedcode size_t NBits() const
    {
        return Nbits;
    }

    // max value of quantize value; 2^Nbits
    cudasharedcode QWordVal QuanRangeEnd() const
    {
        return rangeend;
    }

    // helper: compute the binary log of a power of two (utility function to convert 'Nbits' into 'ldNbits'
    static size_t ld(size_t v)
    {
        if (v == 1)
        {
            return 0;
        }
        else if (v & 1) // not a power of two
        {
            RuntimeError("ld: 'bits' must be a power of two");
        }
        else
        {
            return 1 + ld(v >> 1);
        }
    }

protected:
    // quantize for full ElemType size bits case (special case that allows to bypass quantization, for testing/debugging purposes)
    cudasharedcode QWordVal QuantizeToFullQWord(ElemType u) const
    {
        assert(Nbits == QWordNumBits);

        // we return the bit pattern that encodes the float value
        return *(QWordVal*) &u;
    }

protected:
    // NBits must be power of two
    size_t ldNbits;
    size_t Nbits;

    QWordVal rangeend;

    // quantization range
    ElemType quantimin;
    ElemType quantimax;

    // quantization threshold for 1-bit case
    ElemType quantimid;

    // precomputed factor for quantizing
    ElemType qfactor;

    // and for unquantizing
    ElemType ufactor;
};
}}}

#endif // __VALUE_QUANTIZER_H__
