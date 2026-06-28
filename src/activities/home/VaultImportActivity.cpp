#include "VaultImportActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MAX_RESULTS = 200;
constexpr size_t MAX_DIRS = 400;
}  // namespace

void VaultImportActivity::scan() {
  paths.clear();
  labels.clear();
  dirs.clear();

  std::vector<std::string> stack;
  stack.push_back("/");

  char nameBuf[256];
  while (!stack.empty() && paths.size() < MAX_RESULTS) {
    const std::string dirPath = std::move(stack.back());
    stack.pop_back();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    dir.rewindDirectory();

    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(nameBuf, sizeof(nameBuf));
      const std::string name = nameBuf;
      const bool isDir = f.isDirectory();
      f.close();

      // Skip hidden/dot entries (covers ".", "..", "/.crosspoint") and system dir.
      if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;

      const std::string full = (dirPath == "/") ? "/" + name : dirPath + "/" + name;

      if (isDir) {
        if (full == "/Vault") continue;  // don't descend into the vault itself
        if (stack.size() < MAX_DIRS) stack.push_back(full);
      } else if (FsHelpers::hasMarkdownExtension(name)) {
        paths.push_back(full);
        labels.push_back(name);
        dirs.push_back(dirPath);
        if (paths.size() >= MAX_RESULTS) break;
      }
    }
    dir.close();
  }

  LOG_INF("VAULT", "Import scan: found %u .md file(s)", (unsigned)paths.size());
  for (const auto& p : paths) LOG_DBG("VAULT", "  md: %s", p.c_str());
}

void VaultImportActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  scan();
  requestUpdate();
}

void VaultImportActivity::onExit() {
  Activity::onExit();
  paths.clear();
  labels.clear();
  dirs.clear();
}

void VaultImportActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (paths.empty()) return;
    ActivityResult res{FilePathResult{paths[selectedIndex]}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  const int count = static_cast<int>(paths.size());
  if (count == 0) return;
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

void VaultImportActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Import .md from SD");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (paths.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No .md files found on SD card");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(paths.size()), selectedIndex,
        [this](int index) { return labels[index]; }, [this](int index) { return dirs[index]; });
  }

  const char* nav = paths.empty() ? "" : tr(STR_DIR_UP);
  const char* nav2 = paths.empty() ? "" : tr(STR_DIR_DOWN);
  const char* sel = paths.empty() ? "" : tr(STR_SELECT);
  const auto labelsBtns = mappedInput.mapLabels(tr(STR_BACK), sel, nav, nav2);
  GUI.drawButtonHints(renderer, labelsBtns.btn1, labelsBtns.btn2, labelsBtns.btn3, labelsBtns.btn4);

  renderer.displayBuffer();
}
