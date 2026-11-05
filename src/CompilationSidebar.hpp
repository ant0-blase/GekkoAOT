#pragma once
#include "CompilationQueue.hpp"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;
class CompilationSidebar final : public QWidget {
public:
  explicit CompilationSidebar(CompilationQueue *queue,
                              QWidget *parent = nullptr);
  std::function<void(const QStringList &)> compileRequested;
  std::function<void(const QString &)> playRequested;

private:
  struct Card {
    QWidget *widget;
    QLabel *icon = nullptr;
    QLabel *title;
    QLabel *status;
    QLabel *percent;
    class QProgressBar *progress;
    QPushButton *cancel;
    QPushButton *retry;
    QPushButton *play;
  };
  CompilationQueue *m_queue;
  QMap<quint64, Card> m_cards;
  QVBoxLayout *m_list;
  QLabel *m_summary;
  QLabel *m_empty;
  QPushButton *m_cancelAll;
  QPushButton *m_clear;
  void addCard(quint64 id);
  void updateCard(quint64 id);
  void updateSummary();
  void showLog(quint64 id);
};
