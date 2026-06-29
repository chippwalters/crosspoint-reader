#pragma once

#include "activities/Activity.h"

// Paperbit Fetch home screen. For now it shows the configured single source URL and lets
// the user set/edit it with the on-screen URL keyboard. (Fetching the index + downloading
// documents into /Fetch is the next step.)
class PaperbitFetchActivity final : public Activity {
 public:
  explicit PaperbitFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PaperbitFetch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void promptForUrl();
};
