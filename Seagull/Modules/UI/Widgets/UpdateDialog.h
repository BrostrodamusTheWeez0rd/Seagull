#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QProgressBar;
class QPushButton;
class SgUpdater;

// Themed modal asking whether to install the tool updates the startup check
// found, then showing live progress if accepted. Inherits the app palette/
// stylesheet from Theme::apply like every other widget. The updater lives on
// its own thread; calls go over queued invokes and its signals arrive queued.
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    UpdateDialog(SgUpdater* updater, const QStringList& pending, QWidget* parent = nullptr);

private:
    void startUpdate();   // prompt state -> progress state, kicks off applyUpdates
    void onProgress(const QString& tool, int percent);
    void onFinished(bool allOk);

    SgUpdater* m_updater;

    QLabel*       statusLabel;
    QProgressBar* progressBar;
    QPushButton*  updateBtn;
    QPushButton*  laterBtn;
};
