#include "CompilationSidebar.hpp"
#include "BuildProgress.hpp"
#include "DiscMetadata.hpp"
#include "SettingsStyle.hpp"
#include "ShellStyle.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

CompilationSidebar::CompilationSidebar(CompilationQueue *queue, QWidget *parent)
    : QWidget(parent), m_queue(queue) {
  setObjectName("compilationSidebar");
  setMinimumWidth(310);
  setStyleSheet(
      settingsStyleSheet() +
      "QWidget#compilationSidebar { background: transparent; }"
      "QWidget#compilationCard { background: rgba(255,255,255,7); border: 1px "
      "solid rgba(255,255,255,18); border-radius: 18px; }"
      "QWidget#compilationCard QLabel { border: none; background: transparent; "
      "}"
      "QProgressBar { border: none; background: #242424; border-radius: 3px; "
      "height: 6px; min-height: 6px; }"
      "QProgressBar::chunk { background: #a9a9a9; border-radius: 3px; }"
      "QPushButton { padding: 9px 14px; min-height: 26px; border-radius: 14px; "
      "background: rgba(255,255,255,12); border: 1px solid "
      "rgba(255,255,255,24); }"
      "QPushButton:focus { border-color: #eeeeee; background: "
      "rgba(255,255,255,25); }"
      "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; "
      "}"
      "QLabel#compilationDetail { color: #bcbcbc; font-size: 12px; }");
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(12, 12, 12, 12);
  outer->setSpacing(12);
  auto *add = new QPushButton("Compile games…");
  add->setIcon(shellIcon("cpu"));
  add->setObjectName("addCompilations");
  add->setToolTip("Choose several games to compile without launching them");
  outer->addWidget(add);
  auto *controls = new QHBoxLayout;
  controls->addWidget(new QLabel("Simultaneous builds"), 1);
  auto *parallel = new QSpinBox;
  parallel->setObjectName("parallelCompilations");
  parallel->setRange(1, 8);
  parallel->setValue(queue->maxParallel());
  parallel->setToolTip("Compiler threads are shared between builds. Changes "
                       "affect the next build.");
  controls->addWidget(parallel);
  outer->addLayout(controls);
  m_summary = new QLabel;
  outer->addWidget(m_summary);
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *content = new QWidget;
  m_list = new QVBoxLayout(content);
  m_list->setContentsMargins(0, 0, 0, 0);
  m_list->setSpacing(10);
  m_empty =
      new QLabel("Select games in the library, then\nchoose Compile selected.");
  m_empty->setAlignment(Qt::AlignCenter);
  m_empty->setWordWrap(true);
  m_empty->setStyleSheet("color: #aaaaaa; padding: 24px 6px;");
  m_list->addWidget(m_empty);
  m_list->addStretch();
  scroll->setWidget(content);
  outer->addWidget(scroll, 1);
  auto *footer = new QHBoxLayout;
  m_cancelAll = new QPushButton("Cancel all");
  m_cancelAll->setIcon(shellIcon("x"));
  m_cancelAll->setObjectName("cancelAllCompilations");
  m_clear = new QPushButton("Clear finished");
  m_clear->setIcon(shellIcon("check"));
  m_clear->setObjectName("clearCompilations");
  footer->addWidget(m_cancelAll);
  footer->addWidget(m_clear);
  outer->addLayout(footer);
  connect(add, &QPushButton::clicked, this, [this] {
    const auto paths = shellChooseFiles(this, "Compile games", {}, true);
    if (!paths.isEmpty() && compileRequested)
      compileRequested(paths);
  });
  connect(parallel, &QSpinBox::valueChanged, queue,
          &CompilationQueue::setMaxParallel);
  connect(queue, &CompilationQueue::countsChanged, this, [this, parallel] {
    parallel->setValue(m_queue->maxParallel());
    updateSummary();
  });
  connect(m_cancelAll, &QPushButton::clicked, queue,
          &CompilationQueue::cancelAll);
  connect(m_clear, &QPushButton::clicked, queue,
          &CompilationQueue::clearFinished);
  connect(queue, &CompilationQueue::jobAdded, this,
          &CompilationSidebar::addCard);
  connect(queue, &CompilationQueue::jobChanged, this,
          &CompilationSidebar::updateCard);
  connect(queue, &CompilationQueue::jobRemoved, this, [this](quint64 id) {
    const auto card = m_cards.take(id);
    delete card.widget;
    updateSummary();
  });
  for (auto id : queue->ids())
    addCard(id);
  updateSummary();
}
void CompilationSidebar::addCard(quint64 id) {
  Card card{};
  card.widget = new QWidget;
  card.widget->setObjectName("compilationCard");
  card.widget->setProperty("jobId", QVariant::fromValue(id));
  auto *layout = new QVBoxLayout(card.widget);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);
  card.title = new QLabel;
  card.title->setWordWrap(true);
  card.title->setTextFormat(Qt::PlainText);
  auto font = card.title->font();
  font.setBold(true);
  card.title->setFont(font);
  auto *header = new QHBoxLayout;
  header->setSpacing(12);
  card.icon = new QLabel;
  card.icon->setFixedSize(72, 32);
  card.icon->setAlignment(Qt::AlignCenter);
  header->addWidget(card.icon);
  header->addWidget(card.title, 1);
  layout->addLayout(header);
  card.status = new QLabel;
  card.status->setObjectName("compilationDetail");
  card.status->setTextFormat(Qt::PlainText);
  card.status->setWordWrap(true);
  auto *statusRow = new QHBoxLayout;
  statusRow->addWidget(card.status, 1);
  card.percent = new QLabel;
  card.percent->setObjectName("compilationPercent");
  card.percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  card.percent->setStyleSheet("color:#eeeeee;font-size:13px;font-weight:500;");
  statusRow->addWidget(card.percent);
  layout->addLayout(statusRow);
  card.progress = new QProgressBar;
  card.progress->setRange(0, 100);
  card.progress->setTextVisible(false);
  card.progress->setFixedHeight(4);
  layout->addWidget(card.progress);
  auto *actions = new QHBoxLayout;
  auto *log = new QPushButton("Details");
  log->setObjectName("compilationDetails");
  log->setStyleSheet(
      "QPushButton {background:transparent;border:1px solid "
      "transparent;padding:5px 9px;min-height:24px;} QPushButton:focus "
      "{border-color:#eeeeee;background:rgba(255,255,255,16);}");
  log->setIcon(shellIcon("file-text"));
  actions->addWidget(log);
  actions->addStretch();
  card.retry = new QPushButton("Retry");
  card.retry->setIcon(shellIcon("rotate-cw"));
  card.cancel = new QPushButton("Cancel");
  card.cancel->setIcon(shellIcon("x"));
  card.cancel->setObjectName("cancelCompilation");
  card.play = new QPushButton(shellIcon("play"), "Play");
  card.play->setObjectName("playCompilation");
  actions->addWidget(card.retry);
  actions->addWidget(card.cancel);
  actions->addWidget(card.play);
  layout->addLayout(actions);
  m_list->insertWidget(m_list->count() - 1, card.widget);
  m_cards.insert(id, card);
  connect(log, &QPushButton::clicked, this, [this, id] { showLog(id); });
  connect(card.cancel, &QPushButton::clicked, this,
          [this, id] { m_queue->cancel(id); });
  connect(card.retry, &QPushButton::clicked, this,
          [this, id] { m_queue->retry(id); });
  connect(card.play, &QPushButton::clicked, this, [this, id] {
    if (auto *job = m_queue->job(id); job && playRequested)
      playRequested(job->request.isoPath);
  });
  updateCard(id);
  updateSummary();
}
void CompilationSidebar::updateCard(quint64 id) {
  const auto *job = m_queue->job(id);
  if (!job || !m_cards.contains(id))
    return;
  const auto card = m_cards.value(id);
  card.title->setText(job->title);
  if (card.icon->pixmap().isNull()) {
    QFile file(discMetadataCachePath(
                   QDir(job->request.userDir).absoluteFilePath("../.."),
                   job->request.isoPath) +
               "/meta/opening.bnr");
    QImage banner;
    if (file.open(QIODevice::ReadOnly))
      banner = decodeGameCubeBanner(file.readAll());
    card.icon->setPixmap(banner.isNull() ? shellProjectIcon().pixmap(28, 28)
                                         : QPixmap::fromImage(banner).scaled(
                                               72, 24, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
  }
  card.title->setToolTip(job->request.isoPath);
  card.status->setText(buildProgressLabel(job->detail));
  card.status->setToolTip(job->detail);
  card.percent->setText(QString("%1 %").arg(job->progress));
  const bool running = job->state == CompilationJob::Running;
  card.percent->setVisible(running);
  card.progress->setVisible(!job->isFinished());
  card.progress->setValue(job->progress);
  card.cancel->setVisible(!job->isFinished());
  card.cancel->setEnabled(job->state != CompilationJob::Cancelling);
  card.retry->setVisible(job->state == CompilationJob::Failed ||
                         job->state == CompilationJob::Cancelled);
  card.play->setVisible(job->state == CompilationJob::Completed);
  card.play->setText(job->foreground ? "Resume" : "Play");
}
void CompilationSidebar::updateSummary() {
  const int active = m_queue->activeCount(), pending = m_queue->pendingCount();
  m_summary->setText(
      active || pending
          ? QString("%1 active · %2 queued").arg(active).arg(pending)
          : "No active compilations");
  m_cancelAll->setEnabled(active || pending);
  bool finished = false;
  for (auto id : m_queue->ids())
    finished |= m_queue->job(id)->isFinished();
  m_clear->setEnabled(finished);
  m_empty->setVisible(m_cards.isEmpty());
}
void CompilationSidebar::showLog(quint64 id) {
  const auto *job = m_queue->job(id);
  if (!job)
    return;
  auto *owner = window();
  QPointer<QDialog> previous = shellActiveDialog(owner);
  if (previous)
    previous->hide();
  auto *dialog = new QDialog(owner);
  dialog->setObjectName("compilationLogPanel");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(job->title + " — compilation log");
  dialog->resize(820, 520);
  dialog->setStyleSheet(settingsStyleSheet());
  auto *layout = new QVBoxLayout(dialog);
  auto *text = new QPlainTextEdit;
  text->setObjectName("compilationLog");
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  font.setPixelSize(12);
  text->setFont(font);
  text->setStyleSheet(
      "QPlainTextEdit#compilationLog "
      "{background:rgba(0,0,0,35);color:#d4d4d4;border:1px solid "
      "rgba(255,255,255,18);border-radius:12px;padding:12px;}");
  text->setReadOnly(true);
  text->setMaximumBlockCount(2000);
  text->setPlainText(job->log);
  layout->addWidget(text);
  connect(dialog, &QDialog::finished, owner, [previous] {
    if (previous) {
      previous->show();
      previous->raise();
    }
  });
  connect(m_queue, &CompilationQueue::jobChanged, dialog,
          [queue = m_queue, id, text](quint64 changed) {
            if (changed == id)
              if (const auto *current = queue->job(id)) {
                const auto position = text->textCursor().position();
                text->setPlainText(current->log);
                auto cursor = text->textCursor();
                cursor.setPosition(qMin(position, int(current->log.size())));
                text->setTextCursor(cursor);
              }
          });
  presentShellDialog(*dialog, "Compilation log", shellTVMode(this));
  dialog->show();
}
