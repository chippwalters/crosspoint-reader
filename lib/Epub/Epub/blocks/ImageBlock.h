#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  // fontId is the reader body font, used to label the "[image too large]"
  // placeholder when the source image is too big to decode safely (see
  // ImageBlock.cpp). It is otherwise unused for normal image rendering.
  void render(GfxRenderer& renderer, int fontId, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;

  // Cached oversized-image guard verdict for this instance so the header-only
  // dimension probe runs at most once, not on every one of a page view's ~14
  // band render passes. -1 = not yet probed, 0 = too large (placeholder),
  // 1 = safe to decode. Not serialized (recomputed per instance).
  int8_t decodeVerdict = -1;
};
