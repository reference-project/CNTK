//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include <algorithm>
#include <unordered_map>
#include <random>
#include "ImageSlimTransformers.h"
#include "Config.h"
#include "ConcStack.h"
#include "ImageConfigHelper.h"
#include "StringUtil.h"
#include "ElementTypeUtils.h"

namespace Microsoft {
namespace MSR {
namespace CNTK {

struct ImageSequenceData : DenseSequenceData
{
    cv::Mat m_image;
    // In case we do not copy data - we have to preserve the original sequence.
    SequenceDataPtr m_original;
};

SlimImageTransformerBase::SlimImageTransformerBase(const ConfigParameters& cfg) : m_imageElementType(0)
{
    m_seed = cfg(L"seed", 0u);
}

StreamDescription SlimImageTransformerBase::Transform(const StreamDescription& inputStream)
{
    m_inputStream = inputStream;
    m_outputStream = m_inputStream;

    if (m_inputStream.m_storageType != StorageType::dense)
    {
        LogicError("ImageTransformerBase supports only dense input streams.");
    }

    if (m_inputStream.m_elementType == ElementType::tdouble)
    {
        m_imageElementType = CV_64F;
    }
    else if (m_inputStream.m_elementType == ElementType::tfloat)
    {
        m_imageElementType = CV_32F;
    }
    else
    {
        RuntimeError("Unsupported type");
    }

    return m_outputStream;
}

SequenceDataPtr SlimImageTransformerBase::Transform(SequenceDataPtr sequence)
{
    auto inputSequence = static_cast<const DenseSequenceData&>(*sequence);

    ImageDimensions dimensions(*inputSequence.m_sampleLayout, HWC);
    int columns = static_cast<int>(dimensions.m_width);
    int rows = static_cast<int>(dimensions.m_height);
    int channels = static_cast<int>(dimensions.m_numChannels);

    auto result = std::make_shared<ImageSequenceData>();
    int type = CV_MAKETYPE(m_imageElementType, channels);
    cv::Mat buffer = cv::Mat(rows, columns, type, inputSequence.m_data);
    Apply(sequence->m_id, buffer);
    if (!buffer.isContinuous())
    {
        buffer = buffer.clone();
    }
    else
    {
        result->m_original = sequence;
    }
    assert(buffer.isContinuous());
    result->m_image = buffer;
    result->m_data = buffer.ptr();
    result->m_numberOfSamples = inputSequence.m_numberOfSamples;

    ImageDimensions outputDimensions(buffer.cols, buffer.rows, buffer.channels());
    result->m_sampleLayout = std::make_shared<TensorShape>(outputDimensions.AsTensorShape(HWC));
    return result;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

SlimCropTransformer::SlimCropTransformer(const ConfigParameters& config) : SlimImageTransformerBase(config)
{
    m_cropType = ParseCropType(config(L"cropType", ""));

    floatargvector cropRatio = config(L"cropRatio", "1.0");
    m_cropRatioMin = cropRatio[0];
    m_cropRatioMax = cropRatio[1];

    if (!(0 < m_cropRatioMin && m_cropRatioMin <= 1.0) ||
        !(0 < m_cropRatioMax && m_cropRatioMax <= 1.0) ||
        m_cropRatioMin > m_cropRatioMax)
    {
        RuntimeError("Invalid cropRatio value, must be > 0 and <= 1. cropMin must "
                     "<= cropMax");
    }

    m_jitterType = ParseJitterType(config(L"jitterType", ""));

    if (!config.ExistsCurrent(L"hflip"))
    {
        m_hFlip = m_cropType == CropType::Random;
    }
    else
    {
        m_hFlip = config(L"hflip");
    }
}

void SlimCropTransformer::Apply(size_t id, cv::Mat &mat)
{
    auto seed = GetSeed();
    auto rng = m_rngs.pop_or_create(
        [seed]()
    {
        return std::make_unique<std::mt19937>(seed);
    });

    double ratio = 1;
    switch (m_jitterType)
    {
    case RatioJitterType::None:
        ratio = m_cropRatioMin;
        break;
    case RatioJitterType::UniRatio:
        if (m_cropRatioMin == m_cropRatioMax)
        {
            ratio = m_cropRatioMin;
        }
        else
        {
            ratio = UniRealT(m_cropRatioMin, m_cropRatioMax)(*rng);
            assert(m_cropRatioMin <= ratio && ratio < m_cropRatioMax);
        }
        break;
    default:
        RuntimeError("Jitter type currently not implemented.");
    }

    int viewIndex = m_cropType == CropType::MultiView10 ? (int)(id % 10) : 0;

    mat = mat(GetCropRect(m_cropType, viewIndex, mat.rows, mat.cols, ratio, *rng));
    if ((m_hFlip && std::bernoulli_distribution()(*rng)) ||
        viewIndex >= 5)
    {
        cv::flip(mat, mat, 1);
    }

    m_rngs.push(std::move(rng));
}

SlimCropTransformer::CropType SlimCropTransformer::ParseCropType(const std::string &src)
{
    if (src.empty() || AreEqualIgnoreCase(src, "center"))
    {
        return CropType::Center;
    }

    if (AreEqualIgnoreCase(src, "random"))
    {
        return CropType::Random;
    }

    if (AreEqualIgnoreCase(src, "multiview10"))
    {
        return CropType::MultiView10;
    }

    RuntimeError("Invalid crop type: %s.", src.c_str());
}

SlimCropTransformer::RatioJitterType SlimCropTransformer::ParseJitterType(const std::string &src)
{
    if (src.empty() || AreEqualIgnoreCase(src, "none"))
    {
        return RatioJitterType::None;
    }

    if (AreEqualIgnoreCase(src, "uniratio"))
    {
        return RatioJitterType::UniRatio;
    }

    if (AreEqualIgnoreCase(src, "unilength"))
    {
        return RatioJitterType::UniLength;
    }

    if (AreEqualIgnoreCase(src, "uniarea"))
    {
        return RatioJitterType::UniArea;
    }

    RuntimeError("Invalid jitter type: %s.", src.c_str());
}

cv::Rect SlimCropTransformer::GetCropRect(CropType type, int viewIndex, int crow, int ccol,
                                          double cropRatio, std::mt19937 &rng)
{
    assert(crow > 0);
    assert(ccol > 0);
    assert(0 < cropRatio && cropRatio <= 1.0);

    int cropSize = static_cast<int>(std::min(crow, ccol) * cropRatio);
    int xOff = -1;
    int yOff = -1;
    switch (type)
    {
    case CropType::Center:
        assert(viewIndex == 0);
        xOff = (ccol - cropSize) / 2;
        yOff = (crow - cropSize) / 2;
        break;
    case CropType::Random:
        assert(viewIndex == 0);
        xOff = UniIntT(0, ccol - cropSize)(rng);
        yOff = UniIntT(0, crow - cropSize)(rng);
        break;
    case CropType::MultiView10:
    {
        assert(0 <= viewIndex && viewIndex < 10);
        // 0 - 4: 4 corners + center crop. 5 - 9: same, but with a flip.
        int isubView = viewIndex % 5;
        switch (isubView)
        {
            // top-left
        case 0:
            xOff = 0;
            yOff = 0;
            break;
            // top-right
        case 1:
            xOff = ccol - cropSize;
            yOff = 0;
            break;
            // bottom-left
        case 2:
            xOff = 0;
            yOff = crow - cropSize;
            break;
            // bottom-right
        case 3:
            xOff = ccol - cropSize;
            yOff = crow - cropSize;
            break;
            // center
        case 4:
            xOff = (ccol - cropSize) / 2;
            yOff = (crow - cropSize) / 2;
            break;
        }
        break;
    }
    default:
        assert(false);
    }

    assert(0 <= xOff && xOff <= ccol - cropSize);
    assert(0 <= yOff && yOff <= crow - cropSize);
    return cv::Rect(xOff, yOff, cropSize, cropSize);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

SlimScaleTransformer::SlimScaleTransformer(const ConfigParameters& config) : SlimImageTransformerBase(config)
{
    m_interpMap.emplace("nearest", cv::INTER_NEAREST);
    m_interpMap.emplace("linear", cv::INTER_LINEAR);
    m_interpMap.emplace("cubic", cv::INTER_CUBIC);
    m_interpMap.emplace("lanczos", cv::INTER_LANCZOS4);

    m_imgWidth = config(L"width");
    m_imgHeight = config(L"height");
    m_imgChannels = config(L"channels");

    size_t cfeat = m_imgWidth * m_imgHeight * m_imgChannels;
    if (cfeat == 0 || cfeat > std::numeric_limits<size_t>().max() / 2)
        RuntimeError("Invalid image dimensions.");

    m_interp.clear();
    std::stringstream ss{config(L"interpolations", "")};
    for (std::string token = ""; std::getline(ss, token, ':');)
    {
        // Explicit cast required for GCC.
        std::transform(token.begin(), token.end(), token.begin(),
                       (int(*) (int)) std::tolower);
        StrToIntMapT::const_iterator res = m_interpMap.find(token);
        if (res != m_interpMap.end())
            m_interp.push_back((*res).second);
    }

    if (m_interp.size() == 0)
        m_interp.push_back(cv::INTER_LINEAR);
}

void SlimScaleTransformer::Apply(size_t id, cv::Mat &mat)
{
    UNUSED(id);

    // If matrix has not been converted to the right type, do it now as rescaling
    // requires floating point type.
    if (mat.type() != CV_MAKETYPE(m_imageElementType, m_imgChannels))
    {
        mat.convertTo(mat, m_imageElementType);
    }

    auto seed = GetSeed();
    auto rng = m_rngs.pop_or_create(
        [seed]()
    {
        return std::make_unique<std::mt19937>(seed);
    });


    auto index = UniIntT(0, static_cast<int>(m_interp.size()) - 1)(*rng);
    assert(m_interp.size() > 0);
    cv::resize(
        mat, mat,
        cv::Size(static_cast<int>(m_imgWidth), static_cast<int>(m_imgHeight)), 0,
        0, m_interp[index]);

    m_rngs.push(std::move(rng));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

SlimMeanTransformer::SlimMeanTransformer(const ConfigParameters& config) : SlimImageTransformerBase(config)
{
    std::wstring meanFile = config(L"meanFile", L"");
    if (meanFile.empty())
        m_meanImg.release();
    else
    {
        cv::FileStorage fs;
        // REVIEW alexeyk: this sort of defeats the purpose of using wstring at
        // all...  [fseide] no, only OpenCV has this problem.
        fs.open(msra::strfun::utf8(meanFile).c_str(), cv::FileStorage::READ);
        if (!fs.isOpened())
            RuntimeError("Could not open file: %ls", meanFile.c_str());
        fs["MeanImg"] >> m_meanImg;
        int cchan;
        fs["Channel"] >> cchan;
        int crow;
        fs["Row"] >> crow;
        int ccol;
        fs["Col"] >> ccol;
        if (cchan * crow * ccol !=
            m_meanImg.channels() * m_meanImg.rows * m_meanImg.cols)
            RuntimeError("Invalid data in file: %ls", meanFile.c_str());
        fs.release();
        m_meanImg = m_meanImg.reshape(cchan, crow);
    }
}

void SlimMeanTransformer::Apply(size_t id, cv::Mat &mat)
{
    UNUSED(id);
    assert(m_meanImg.size() == cv::Size(0, 0) ||
           (m_meanImg.size() == mat.size() &&
           m_meanImg.channels() == mat.channels()));

    // REVIEW alexeyk: check type conversion (float/double).
    if (m_meanImg.size() == mat.size())
    {
        mat = mat - m_meanImg;
    }
}

SlimTransposeTransformer::SlimTransposeTransformer(const ConfigParameters&)
{
}

StreamDescription SlimTransposeTransformer::Transform(const StreamDescription& inputStream)
{
    m_inputStream = inputStream;

    ImageDimensions dimensions(*m_inputStream.m_sampleLayout, HWC);

    // Changing from NHWC to NCHW
    m_outputStream = m_inputStream;
    m_outputStream.m_sampleLayout = std::make_shared<TensorShape>(dimensions.AsTensorShape(CHW));

    if (m_inputStream.m_storageType != StorageType::dense)
    {
        LogicError("Transpose transformer supports only dense streams.");
    }

    return m_outputStream;
}

// Transformation of the sequence.
SequenceDataPtr SlimTransposeTransformer::Transform(SequenceDataPtr sequence)
{
    if (m_inputStream.m_elementType == ElementType::tdouble)
    {
        return TypedApply<double>(sequence);
    }

    if (m_inputStream.m_elementType == ElementType::tfloat)
    {
        return TypedApply<float>(sequence);
    }

    RuntimeError("Unsupported type");
}

// The class represents a sequence that owns an internal data buffer.
// Passed from the TransposeTransformer.
// TODO: Trasposition potentially could be done in place.
struct DenseSequenceWithBuffer : DenseSequenceData
{
    std::vector<char> m_buffer;
};

template <class TElemType>
SequenceDataPtr SlimTransposeTransformer::TypedApply(SequenceDataPtr sequence)
{
    auto inputSequence = static_cast<DenseSequenceData&>(*sequence);
    assert(inputSequence.m_numberOfSamples == 1);

    size_t count = m_inputStream.m_sampleLayout->GetNumElements() * GetSizeByType(m_inputStream.m_elementType);

    auto result = std::make_shared<DenseSequenceWithBuffer>();
    result->m_buffer.resize(count);

    ImageDimensions dimensions(*m_inputStream.m_sampleLayout, ImageLayoutKind::HWC);
    size_t rowCount = dimensions.m_height * dimensions.m_width;
    size_t channelCount = dimensions.m_numChannels;

    auto src = reinterpret_cast<TElemType*>(inputSequence.m_data);
    auto dst = reinterpret_cast<TElemType*>(result->m_buffer.data());

    for (size_t irow = 0; irow < rowCount; irow++)
    {
        for (size_t icol = 0; icol < channelCount; icol++)
        {
            dst[icol * rowCount + irow] = src[irow * channelCount + icol];
        }
    }

    result->m_sampleLayout = m_outputStream.m_sampleLayout;
    result->m_data = result->m_buffer.data();
    result->m_numberOfSamples = inputSequence.m_numberOfSamples;
    return result;
}

}}}