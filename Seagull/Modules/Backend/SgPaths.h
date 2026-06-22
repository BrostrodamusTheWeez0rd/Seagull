#pragma once

#include <QString>

// Central resolver for the per-media-type save folders (Settings -> Folders).
// Everything that writes files asks here, so the config keys, defaults, and the
// legacy Paths/DownloadFolder fallback live in exactly one place.
//
// "Unify media folders" (Paths/UnifyMedia) collapses the four typed folders
// into one (Paths/UnifiedFolder): every typed accessor returns it, and the
// Library's type buttons turn into pure extension filters over that folder.
// Pass honourUnify=false to read a type's own configured folder regardless
// (the Settings page does, so the per-type rows keep their values).
namespace SgPaths {
    QString configDir();       // Where config.ini and history files live (Config/ subfolder)
    QString configFile();      // Convenience: configDir() + "/config.ini"

    QString homeFolder();      // File Explorer's starting directory

    // Where yt-dlp downloads land. Dedicated and deliberately OUTSIDE the unify
    // system — downloads are incoming files, not the curated media folders.
    QString downloadFolder();

    // "Smart sort downloading": when on (default), a download is routed into its
    // media-type folder (video -> Videos, audio -> Audio); when off, everything
    // lands in the single downloadFolder().
    bool    smartSortDownloads();

    QString videoFolder(bool honourUnify = true);     // saved videos
    QString audioFolder(bool honourUnify = true);     // saved audio
    QString photoFolder(bool honourUnify = true);     // saved images
    QString recordingFolder(bool honourUnify = true); // recordings + clips
    QString playlistFolder(bool honourUnify = true);  // saved .sgpl playlists

    bool    unifyMedia();      // Paths/UnifyMedia — one folder for all media?
    QString unifiedFolder();   // that one folder
}
