/*****************************************************************************************
 * SGCT                                                                                  *
 * Simple Graphics Cluster Toolkit                                                       *
 *                                                                                       *
 * Copyright (c) 2012-2022                                                               *
 * For conditions of distribution and use, see copyright notice in LICENSE.md            *
 ****************************************************************************************/

#ifndef __SGCT__CORRECTION_SCALABLE__H__
#define __SGCT__CORRECTION_SCALABLE__H__

#include <sgct/math.h>
#include <sgct/correction/buffer.h>
#include <filesystem>

namespace sgct { class BaseViewport; }

namespace sgct::correction {

Buffer generateScalableMesh(const std::filesystem::path& path, BaseViewport& parent);

} // namespace sgct::correction

#endif // __SGCT__CORRECTION_SCALABLE__H__
