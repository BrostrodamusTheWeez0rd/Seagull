#pragma once
#include <QString>
#include <QStringList>

// yt-dlp option/argument policy, read from config.ini. Keeps all the
// settings-to-flags mapping out of the process wrapper.
namespace SgOptions {

    // Builds the yt-dlp args for a download (Download Type/Format/Quality + the
    // destination folder, all from config.ini).
    QStringList buildDownloadArgs(const QString& url);

    // Streaming height cap from the "Streaming/Quality" setting. "Best Available"
    // (or unset) -> -1 (no cap); "1080p" / "2160p (4K)" -> 1080 / 2160.
    int defaultStreamHeight();

    // yt-dlp `--cookies-from-browser <b>` args from the "Streaming/CookiesBrowser"
    // setting, or empty when it's "None". Authenticated requests are challenged as
    // a bot far less often. Appended (before the URL) to every yt-dlp invocation.
    QStringList cookieArgs();

}
