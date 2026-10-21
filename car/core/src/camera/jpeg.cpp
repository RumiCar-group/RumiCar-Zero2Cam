#include "jpeg.hpp"

#include <stdexcept>
#include <string>

#include <turbojpeg.h>

namespace rumicar::cam {

JpegEncoder::JpegEncoder() {
    handle_ = tjInitCompress();
    if (!handle_) throw std::runtime_error("tjInitCompress failed");
}

JpegEncoder::~JpegEncoder() {
    if (handle_) tjDestroy(handle_);
}

std::vector<uint8_t> JpegEncoder::encode(const uint8_t* rgb, uint32_t width, uint32_t height, uint32_t stride, int quality) {
    if (!rgb || width == 0 || height == 0 || stride < width * 3) throw std::runtime_error("jpeg: bad image geometry");
    if (quality < 1 || quality > 100) throw std::runtime_error("jpeg: quality must be 1..100");
    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    const int rc = tjCompress2(handle_, rgb, static_cast<int>(width), static_cast<int>(stride), static_cast<int>(height), TJPF_RGB,
                               &out, &out_size, TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (rc != 0) {
        const std::string msg = tjGetErrorStr2(handle_);
        if (out) tjFree(out);
        throw std::runtime_error("jpeg: " + msg);
    }
    std::vector<uint8_t> result(out, out + out_size);
    tjFree(out);
    return result;
}

void jpeg_size(const std::vector<uint8_t>& jpeg, uint32_t& width, uint32_t& height) {
    tjhandle h = tjInitDecompress();
    if (!h) throw std::runtime_error("tjInitDecompress failed");
    int w = 0, hgt = 0, subsamp = 0, colorspace = 0;
    const int rc = tjDecompressHeader3(h, jpeg.data(), static_cast<unsigned long>(jpeg.size()), &w, &hgt, &subsamp, &colorspace);
    const std::string msg = rc != 0 ? tjGetErrorStr2(h) : "";
    tjDestroy(h);
    if (rc != 0) throw std::runtime_error("jpeg: " + msg);
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(hgt);
}

DecodedImage jpeg_decode(const std::vector<uint8_t>& jpeg) {
    tjhandle h = tjInitDecompress();
    if (!h) throw std::runtime_error("tjInitDecompress failed");
    int w = 0, hgt = 0, subsamp = 0, colorspace = 0;
    int rc = tjDecompressHeader3(h, jpeg.data(), static_cast<unsigned long>(jpeg.size()), &w, &hgt, &subsamp, &colorspace);
    if (rc != 0 || w <= 0 || hgt <= 0) {
        const std::string msg = tjGetErrorStr2(h);
        tjDestroy(h);
        throw std::runtime_error("jpeg: " + msg);
    }
    DecodedImage img;
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(hgt);
    img.rgb.resize(static_cast<size_t>(img.width) * img.height * 3);
    rc = tjDecompress2(h, jpeg.data(), static_cast<unsigned long>(jpeg.size()), img.rgb.data(), w, static_cast<int>(img.width * 3), hgt,
                       TJPF_RGB, TJFLAG_ACCURATEDCT);
    const std::string msg = rc != 0 ? tjGetErrorStr2(h) : "";
    tjDestroy(h);
    if (rc != 0) throw std::runtime_error("jpeg: " + msg);
    return img;
}

} // namespace rumicar::cam
