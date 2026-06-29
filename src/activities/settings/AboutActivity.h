#pragma once

#include "activities/Activity.h"

// A read-only "About" screen (Settings -> System -> About): attribution for the
// Paperbit fork plus a scannable QR code to the support Discord.
class AboutActivity final : public Activity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("About", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
