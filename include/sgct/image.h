/*****************************************************************************************
 * SGCT                                                                                  *
 * Simple Graphics Cluster Toolkit                                                       *
 *                                                                                       *
 * Copyright (c) 2012-2024                                                               *
 * For conditions of distribution and use, see copyright notice in LICENSE.md            *
 ****************************************************************************************/

#ifndef __SGCT__IMAGE__H__
#define __SGCT__IMAGE__H__

#include <sgct/sgctexports.h>
#include <sgct/math.h>
#include <filesystem>

namespace sgct {

class SGCT_EXPORT Image {
public:
    Image() = default;
    ~Image();

    void allocateOrResizeData();
    void load(const std::filesystem::path& filename);
    void load(unsigned char* data, int length);

    /**
     * Save the buffer to file. Type is automatically set by filename suffix.
     */
    void save(const std::filesystem::path& filename);

    unsigned char* data();
    const unsigned char* data() const;
    int channels() const;
    int bytesPerChannel() const;
    ivec2 size() const;

    void setSize(ivec2 size);
    void setChannels(int channels);
    void setBytesPerChannel(int bpc);

private:
    int _nChannels = 0;
    ivec2 _size = ivec2{ 0, 0 };
    unsigned int _dataSize = 0;
    int _bytesPerChannel = 1;
    unsigned char* _data = nullptr;
};

} // namespace sgct

#endif // __SGCT__IMAGE__H__
