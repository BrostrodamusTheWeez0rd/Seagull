#include "SgSpellCheck.h"

#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>
#include <cwchar>

SgSpellCheck::SgSpellCheck(QObject* parent) : QObject(parent) {
    // Qt's GUI thread is already an STA; CoInitializeEx is refcounted, so this
    // succeeds (S_FALSE) when the apartment is already up. We only balance with
    // CoUninitialize if our call actually took a reference (SUCCEEDED). If the
    // apartment was initialized in a different mode the call fails harmlessly and
    // we leave COM teardown to whoever set it up.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(hrInit);

    if (FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_factory)))) {
        m_factory = nullptr;
        return;
    }

    // Detect the user's UI language as a BCP-47 tag; fall back to en-US.
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0)
        wcscpy_s(locale, L"en-US");

    BOOL supported = FALSE;
    if (FAILED(m_factory->IsSupported(locale, &supported)) || !supported) {
        // Detected language unsupported: try en-US before giving up entirely.
        if (FAILED(m_factory->IsSupported(L"en-US", &supported)) || !supported)
            return;
        wcscpy_s(locale, L"en-US");
    }

    if (FAILED(m_factory->CreateSpellChecker(locale, &m_checker)))
        m_checker = nullptr;
}

SgSpellCheck::~SgSpellCheck() {
    if (m_checker) m_checker->Release();
    if (m_factory) m_factory->Release();
    if (m_comInitialized) CoUninitialize();
}

QVector<SpellError> SgSpellCheck::check(const QString& text) const {
    QVector<SpellError> out;
    if (!m_checker || text.isEmpty()) return out;

    IEnumSpellingError* errs = nullptr;
    if (FAILED(m_checker->Check(reinterpret_cast<LPCWSTR>(text.utf16()), &errs)) || !errs)
        return out;

    ISpellingError* err = nullptr;
    while (errs->Next(&err) == S_OK && err) {
        ULONG start = 0, len = 0;
        err->get_StartIndex(&start);
        err->get_Length(&len);
        if (len > 0)
            out.push_back({ static_cast<int>(start), static_cast<int>(len) });
        err->Release();
        err = nullptr;
    }
    errs->Release();
    return out;
}

QStringList SgSpellCheck::suggest(const QString& word) const {
    QStringList out;
    if (!m_checker || word.isEmpty()) return out;

    IEnumString* en = nullptr;
    if (FAILED(m_checker->Suggest(reinterpret_cast<LPCWSTR>(word.utf16()), &en)) || !en)
        return out;

    LPOLESTR s = nullptr;
    ULONG fetched = 0;
    while (en->Next(1, &s, &fetched) == S_OK && fetched == 1 && s) {
        out << QString::fromWCharArray(s);
        CoTaskMemFree(s);
        s = nullptr;
    }
    en->Release();
    return out;
}

void SgSpellCheck::addToDictionary(const QString& word) {
    if (m_checker && !word.isEmpty())
        m_checker->Add(reinterpret_cast<LPCWSTR>(word.utf16()));
}

void SgSpellCheck::ignore(const QString& word) {
    if (m_checker && !word.isEmpty())
        m_checker->Ignore(reinterpret_cast<LPCWSTR>(word.utf16()));
}
