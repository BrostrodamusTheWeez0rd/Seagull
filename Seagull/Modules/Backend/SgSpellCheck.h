#ifndef SGSPELLCHECK_H
#define SGSPELLCHECK_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// A misspelled range within a string. start/length are QString indices (UTF-16
// code units), which match the OS API's indices 1:1 since QString is UTF-16.
struct SpellError {
    int start;
    int length;
};

// Forward-declare the COM interfaces so <spellcheck.h> never leaks into headers
// that include this. (MIDL interfaces are structs.)
struct ISpellCheckerFactory;
struct ISpellChecker;

// Qt wrapper around the Windows Spell Checking API (ISpellChecker, available
// since Windows 8). Peer to SgYtDlp/SgSearch. This is a GUI-thread object: the
// checks run on short single-line strings, so the synchronous in-proc COM calls
// are cheap and don't need a worker thread. No COM types leak out of this header.
//
// If COM init fails or the OS reports the detected language unsupported, the
// object is inert (isAvailable() == false, check()/suggest() return empty), so
// the text fields simply behave as plain inputs.
class SgSpellCheck : public QObject {
    Q_OBJECT
public:
    explicit SgSpellCheck(QObject* parent = nullptr);
    ~SgSpellCheck() override;

    bool isAvailable() const { return m_checker != nullptr; }

    // Misspelled ranges in text (empty when unavailable or all-correct).
    QVector<SpellError> check(const QString& text) const;
    // Suggested corrections for a single word, best first.
    QStringList suggest(const QString& word) const;

    void addToDictionary(const QString& word); // persists in the Windows user dictionary
    void ignore(const QString& word);          // session-only ignore

private:
    ISpellCheckerFactory* m_factory = nullptr;
    ISpellChecker*        m_checker = nullptr;
    bool                  m_comInitialized = false; // true only if our CoInitializeEx owns a ref
};

#endif // SGSPELLCHECK_H
