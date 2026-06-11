#pragma once

#include <QString>

// Central resolver for the per-media-type save folders (Settings -> Folders).
// Everything that writes files asks here, so the config keys, defaults, and the
// legacy Paths/DownloadFolder fallback live in exactly one place.
namespace SgPaths {
    QString homeFolder();      // Library's starting directory
    QString videoFolder();     // video downloads
    QString audioFolder();     // audio downloads / extractions
    QString photoFolder();     // saved images
    QString recordingFolder(); // recordings + clips
}
