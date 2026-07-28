/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2018 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "home_screen_widget.h"
#include "gui/action_icon.h"

#include <QAbstractButton>
#include <QApplication> // IWYU pragma: keep
#include <QCheckBox>
#include <QCommandLinkButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QSettings>
#include <QSize>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/document_path.h"
#include "core/storage_location.h" // IWYU pragma: keep
#include "fileformats/file_format_registry.h"
#include "gui/home_screen_controller.h"
#include "gui/main_window.h"
#include "gui/settings_dialog.h"
#include "gui/util_gui.h"
#include "settings.h"

namespace OpenOrienteering {

// ### AbstractHomeScreenWidget ###

AbstractHomeScreenWidget::AbstractHomeScreenWidget(
    HomeScreenController *controller, QWidget *parent)
    : QWidget(parent), controller(controller) {
  Q_ASSERT(controller->getWindow());
}

AbstractHomeScreenWidget::~AbstractHomeScreenWidget() {
  // nothing
}

QLabel *AbstractHomeScreenWidget::makeHeadline(const QString &text,
                                               QWidget *parent) const {
  QLabel *title_label = new QLabel(text, parent);
  QFont title_font = title_label->font();
  int pixel_size = title_font.pixelSize();
  if (pixel_size > 0) {
    title_font.setPixelSize(pixel_size * 2);
  } else {
    pixel_size = title_font.pointSize();
    title_font.setPointSize(pixel_size * 2);
  }
  title_font.setBold(true);
  title_label->setFont(title_font);
  title_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  return title_label;
}

QAbstractButton *AbstractHomeScreenWidget::makeButton(const QString &text,
                                                      QWidget *parent) const {
  QAbstractButton *button = new QCommandLinkButton(text, parent);
  QFont button_font = button->font();
  int pixel_size = button_font.pixelSize();
  if (pixel_size > 0) {
    button_font.setPixelSize(pixel_size * 3 / 2);
  } else {
    pixel_size = button_font.pointSize();
    button_font.setPointSize(pixel_size * 3 / 2);
  }
  button->setFont(button_font);
  return button;
}

QAbstractButton *AbstractHomeScreenWidget::makeButton(const QString &text,
                                                      const QIcon &icon,
                                                      QWidget *parent) const {
  QAbstractButton *button = makeButton(text, parent);
  button->setIcon(icon);
  return button;
}

// ### HomeScreenWidgetDesktop ###

HomeScreenWidgetDesktop::HomeScreenWidgetDesktop(
    HomeScreenController *controller, QWidget *parent)
    : AbstractHomeScreenWidget(controller, parent) {
  QLabel *title_label =
      new QLabel(QString::fromLatin1("<img src=\":/images/title.png\"/>"));
  title_label->setAlignment(Qt::AlignCenter);
  QWidget *menu_widget = makeMenuWidget(controller, parent);
  QWidget *recent_files_widget = makeRecentFilesWidget(controller, parent);
  QWidget *tips_widget = makeTipsWidget(controller, parent);

  QGridLayout *layout = new QGridLayout();
  layout->setSpacing(2 * layout->spacing());
  layout->addWidget(title_label, 0, 0, 1, 2);
  layout->addWidget(menu_widget, 1, 0, 2, 1);
  layout->addWidget(recent_files_widget, 1, 1);
  layout->setRowStretch(1, 4);
  layout->addWidget(tips_widget, 2, 1);
  layout->setRowStretch(2, 3);
  setLayout(layout);

  setAutoFillBackground(false);
}

HomeScreenWidgetDesktop::~HomeScreenWidgetDesktop() {
  // nothing
}

QWidget *
HomeScreenWidgetDesktop::makeMenuWidget(HomeScreenController *controller,
                                        QWidget *parent) {
  MainWindow *window = controller->getWindow();

  QVBoxLayout *menu_layout = new QVBoxLayout();

  QLabel *menu_headline = makeHeadline(tr("Activities"));
  menu_layout->addWidget(menu_headline);
  QAbstractButton *button_new_map =
      makeButton(tr("Create a new map ..."), ActionIcon::fromName(u"new"));
  menu_layout->addWidget(button_new_map);
  QAbstractButton *button_open_map =
      makeButton(tr("Open map ..."), ActionIcon::fromName(u"open"));
  menu_layout->addWidget(button_open_map);
  QAbstractButton *button_map_hub =
      makeButton(tr("Map Hub — library and my work…"));
  menu_layout->addWidget(button_map_hub);

  menu_layout->addStretch(1);

  auto *button_touch =
      makeButton(tr("Touch mode"), ActionIcon::fromName(u"tool-touch-cursor"));
  button_touch->setCheckable(true);
  button_touch->setChecked(Settings::getInstance().touchModeEnabled());
  menu_layout->addWidget(button_touch);
  QAbstractButton *button_settings =
      makeButton(tr("Settings"), ActionIcon::fromName(u"settings"));
  menu_layout->addWidget(button_settings);
  QAbstractButton *button_about =
      makeButton(tr("About %1", "As in 'About OpenOrienteering Mapper'")
                     .arg(window->appName()),
                 ActionIcon::fromName(u"about"));
  menu_layout->addWidget(button_about);
  QAbstractButton *button_help =
      makeButton(tr("Help"), ActionIcon::fromName(u"help"));
  menu_layout->addWidget(button_help);
  QAbstractButton *button_exit = makeButton(
      tr("Exit"), style()->standardIcon(QStyle::SP_DialogCloseButton));
  menu_layout->addWidget(button_exit);

  connect(button_new_map, &QAbstractButton::clicked, window,
          &MainWindow::showNewMapWizard);
  connect(button_open_map, &QAbstractButton::clicked, window,
          &MainWindow::showOpenDialog);
  connect(button_map_hub, &QAbstractButton::clicked, window,
          &MainWindow::showMapHub);
  connect(button_touch, &QAbstractButton::toggled, this, [](bool enabled) {
    Settings::getInstance().setTouchModeEnabled(enabled);
  });
  connect(button_settings, &QAbstractButton::clicked, window,
          &MainWindow::showSettings);
  connect(button_about, &QAbstractButton::clicked, window,
          &MainWindow::showAbout);
  connect(button_help, &QAbstractButton::clicked, window,
          &MainWindow::showHelp);
  connect(button_exit, &QAbstractButton::clicked, qApp,
          &QApplication::closeAllWindows);

  QWidget *menu_widget = new QWidget(parent);
  menu_widget->setLayout(menu_layout);
  menu_widget->setAutoFillBackground(true);
  return menu_widget;
}

QWidget *
HomeScreenWidgetDesktop::makeRecentFilesWidget(HomeScreenController *controller,
                                               QWidget *parent) {
  QGridLayout *recent_files_layout = new QGridLayout();

  QLabel *recent_files_headline = makeHeadline(tr("Recent maps"));
  recent_files_layout->addWidget(recent_files_headline, 0, 0, 1, 2);

  recent_files_list = new QListWidget();
  QFont list_font = recent_files_list->font();
  int pixel_size = list_font.pixelSize();
  if (pixel_size > 0) {
    list_font.setPixelSize(pixel_size * 3 / 2);
  } else {
    pixel_size = list_font.pointSize();
    list_font.setPointSize(pixel_size * 3 / 2);
  }
  recent_files_list->setFont(list_font);
  recent_files_list->setSpacing(pixel_size / 2);
  recent_files_list->setCursor(Qt::PointingHandCursor);
  recent_files_list->setStyleSheet(QString::fromLatin1(" \
	  QListWidget::item:hover { \
	    color: palette(highlighted-text); \
	    background: palette(highlight); \
	  } "));
  recent_files_layout->addWidget(recent_files_list, 1, 0, 1, 2);

  open_mru_file_check =
      new QCheckBox(tr("Open most recently used file on start"));
  recent_files_layout->addWidget(open_mru_file_check, 2, 0, 1, 1);

  QPushButton *clear_list_button = new QPushButton(tr("Clear list"));
  recent_files_layout->addWidget(clear_list_button, 2, 1, 1, 1);

  recent_files_layout->setRowStretch(1, 1);
  recent_files_layout->setColumnStretch(0, 1);

  connect(recent_files_list, &QListWidget::itemClicked, this,
          &HomeScreenWidgetDesktop::recentFileClicked);
  connect(open_mru_file_check, &QAbstractButton::clicked, controller,
          &HomeScreenController::setOpenMRUFile);
  connect(clear_list_button, &QAbstractButton::clicked, controller,
          &HomeScreenController::clearRecentFiles);

  QWidget *recent_files_widget = new QWidget(parent);
  recent_files_widget->setLayout(recent_files_layout);
  recent_files_widget->setAutoFillBackground(true);
  return recent_files_widget;
}

QWidget *
HomeScreenWidgetDesktop::makeTipsWidget(HomeScreenController *controller,
                                        QWidget *parent) {
  QGridLayout *tips_layout = new QGridLayout();
  QWidget *tips_headline = makeHeadline(tr("Tip of the day"));
  tips_layout->addWidget(tips_headline, 0, 0, 1, 3);
  tips_label = new QLabel();
  tips_label->setWordWrap(true);
  tips_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  tips_check = new QCheckBox(tr("Show tip of the day"));
  tips_check->setChecked(true);
  tips_layout->addWidget(tips_check, 2, 0, 1, 1);
  tips_layout->addWidget(tips_label, 1, 0, 1, 3);
  QPushButton *prev_button =
      new QPushButton(ActionIcon::fromName(u"arrow-left"), tr("Previous"));
  tips_layout->addWidget(prev_button, 2, 1, 1, 1);
  QPushButton *next_button =
      new QPushButton(ActionIcon::fromName(u"arrow-right"), tr("Next"));
  tips_layout->addWidget(next_button, 2, 2, 1, 1);

  tips_layout->setRowStretch(1, 1);
  tips_layout->setColumnStretch(0, 1);

  tips_children.reserve(4);
  tips_children.push_back(tips_headline);
  tips_children.push_back(tips_label);
  tips_children.push_back(prev_button);
  tips_children.push_back(next_button);

  MainWindow *window = controller->getWindow();
  connect(tips_label, &QLabel::linkActivated, window, &MainWindow::linkClicked);
  connect(tips_check, &QAbstractButton::clicked, controller,
          &HomeScreenController::setTipsVisible);
  connect(prev_button, &QAbstractButton::clicked, controller,
          &HomeScreenController::goToPreviousTip);
  connect(next_button, &QAbstractButton::clicked, controller,
          &HomeScreenController::goToNextTip);

  QWidget *tips_widget = new QWidget(parent);
  tips_widget->setLayout(tips_layout);
  tips_widget->setAutoFillBackground(true);
  return tips_widget;
}

void HomeScreenWidgetDesktop::setRecentFiles(const QStringList &files) {
  recent_files_list->clear();
  for (auto &&file : files) {
    QListWidgetItem *new_item =
        new QListWidgetItem(DocumentPath::displayName(file));
    new_item->setData(pathRole(), file);
    new_item->setToolTip(file);
    recent_files_list->addItem(new_item);
  }
}

void HomeScreenWidgetDesktop::recentFileClicked(QListWidgetItem *item) {
  setEnabled(false);
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100 /* ms */);
  QString path = item->data(pathRole()).toString();
  controller->getWindow()->openPath(path);
  setEnabled(true);
}

void HomeScreenWidgetDesktop::paintEvent(QPaintEvent *) {
  // Background
  QPainter p(this);
  p.setPen(Qt::NoPen);
  p.setBrush(Qt::gray);
  p.drawRect(rect());
}

void HomeScreenWidgetDesktop::setOpenMRUFileChecked(bool state) {
  open_mru_file_check->setChecked(state);
}

void HomeScreenWidgetDesktop::setTipOfTheDay(const QString &text) {
  tips_label->setText(text);
}

void HomeScreenWidgetDesktop::setTipsVisible(bool state) {
  QGridLayout *layout = qobject_cast<QGridLayout *>(this->layout());
  for (auto widget : tips_children) {
    widget->setVisible(state);
  }
  if (layout)
    layout->setRowStretch(2, state ? 3 : 0);

  tips_check->setChecked(state);
}

// ### HomeScreenWidgetMobile ###

HomeScreenWidgetMobile::HomeScreenWidgetMobile(HomeScreenController *controller,
                                               QWidget *parent)
    : AbstractHomeScreenWidget(controller, parent) {
#if defined(Q_OS_IOS)
  auto *scroll_area = new QScrollArea(this);
  scroll_area->setWidgetResizable(true);
  scroll_area->setFrameShape(QFrame::NoFrame);
  scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  QScroller::grabGesture(scroll_area->viewport(), QScroller::TouchGesture);

  auto *content = new QWidget(scroll_area);
  auto *content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(18, 18, 18, 24);
  content_layout->setSpacing(8);

  auto *title = new QLabel(tr("Mapper"), content);
  auto title_font = title->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 2.0);
  title_font.setWeight(QFont::Bold);
  title->setFont(title_font);
  content_layout->addWidget(title);

  auto *subtitle = new QLabel(tr("Continue a map, open fieldwork from Map Hub, "
                                 "or start something new."),
                              content);
  subtitle->setWordWrap(true);
  subtitle->setStyleSheet(QStringLiteral("color: palette(mid);"));
  content_layout->addWidget(subtitle);
  content_layout->addSpacing(12);

  auto make_section_title = [content](const QString &text) {
    auto *label = new QLabel(text, content);
    auto font = label->font();
    font.setBold(true);
    font.setPointSizeF(17.0);
    label->setFont(font);
    return label;
  };

  auto make_action = [content](const QString &title_text,
                               const QString &description, const QIcon &icon) {
    auto *button = new QCommandLinkButton(title_text, description, content);
    auto action_font = button->font();
    action_font.setPointSizeF(16.0);
    button->setFont(action_font);
    button->setIcon(icon);
    button->setIconSize(QSize(30, 30));
    button->setMinimumHeight(88);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    button->setStyleSheet(QStringLiteral(
        "QCommandLinkButton { background: transparent; border: 0; "
        "border-radius: 0; padding: 10px 12px; text-align: left; }"
        "QCommandLinkButton:pressed { background: palette(alternate-base); }"));
    return button;
  };

  auto make_action_group =
      [content](const QList<QCommandLinkButton *> &actions) {
        auto *group = new QFrame(content);
        group->setObjectName(QStringLiteral("startupActionGroup"));
        group->setStyleSheet(QStringLiteral(
            "QFrame#startupActionGroup { background: palette(base); "
            "border: 1px solid palette(midlight); border-radius: 14px; }"));
        auto *group_layout = new QVBoxLayout(group);
        group_layout->setContentsMargins({});
        group_layout->setSpacing(0);
        for (int i = 0; i < actions.size(); ++i) {
          group_layout->addWidget(actions.at(i));
          if (i + 1 == actions.size())
            continue;
          auto *separator = new QFrame(group);
          separator->setFrameShape(QFrame::NoFrame);
          separator->setFixedHeight(1);
          separator->setStyleSheet(
              QStringLiteral("background: palette(midlight); border: 0;"));
          group_layout->addWidget(separator);
        }
        return group;
      };

  content_layout->addWidget(make_section_title(tr("Continue")));
  recent_files_empty_label = new QLabel(
      tr("No recent maps yet. Open a map from Files or start a new one."),
      content);
  recent_files_empty_label->setWordWrap(true);
  recent_files_empty_label->setStyleSheet(
      QStringLiteral("padding: 14px; color: palette(mid); "
                     "background: palette(base); border: 1px solid "
                     "palette(midlight); border-radius: 14px;"));
  content_layout->addWidget(recent_files_empty_label);

  file_list_widget = makeFileListWidget();
  file_list_widget->setFrameShape(QFrame::NoFrame);
  file_list_widget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  file_list_widget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  file_list_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  file_list_widget->setStyleSheet(QStringLiteral(
      "QListWidget { background: palette(base); border: 1px solid "
      "palette(midlight); border-radius: 14px; }"
      "QListWidget::item { padding: 12px 8px; border-bottom: 1px solid "
      "palette(midlight); }"));
  connect(file_list_widget, &QListWidget::itemClicked, this,
          &HomeScreenWidgetMobile::itemClicked);
  content_layout->addWidget(file_list_widget);

  content_layout->addSpacing(10);
  content_layout->addWidget(make_section_title(tr("Open or start")));
  auto *map_hub_button = make_action(
      tr("Map Hub"),
      tr("Open an assignment, organization map, or event workspace."),
      ActionIcon::fromName(u"map-information"));
  auto *open_button =
      make_action(tr("Browse Files"),
                  tr("Open an existing OMAP, OCD, or other supported map."),
                  ActionIcon::fromName(u"open"));
  auto *new_button = make_action(
      tr("Create a local map"),
      tr("Choose a map scale and symbol standard, then save it in Files."),
      ActionIcon::fromName(u"new"));
  connect(map_hub_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showMapHub);
  connect(open_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showOpenDialog);
  connect(new_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showNewMapWizard);
  content_layout->addWidget(
      make_action_group({map_hub_button, open_button, new_button}));

#ifdef MAPPER_GNSS_AVAILABLE
  content_layout->addSpacing(10);
  content_layout->addWidget(make_section_title(tr("Ready for the field")));
  auto gnss_description = tr("Use iPhone location, or configure a Bluetooth "
                             "GNSS receiver and NTRIP corrections.");
  const auto &settings = Settings::getInstance();
  if (!settings.gnssDeviceName().isEmpty())
    gnss_description =
        settings.gnssAutoStartNtrip()
            ? tr("%1 with NTRIP corrections.").arg(settings.gnssDeviceName())
            : tr("%1 without automatic corrections.")
                  .arg(settings.gnssDeviceName());
  gnss_button = make_action(tr("GNSS && corrections"), gnss_description,
                            ActionIcon::fromName(u"tool-gps-display"));
  connect(gnss_button, &QAbstractButton::clicked, this,
          &HomeScreenWidgetMobile::showGnssSettings);
  content_layout->addWidget(make_action_group({gnss_button}));
#endif

  content_layout->addSpacing(10);
  content_layout->addWidget(make_section_title(tr("Learn and configure")));
  auto *examples_button =
      make_action(tr("Example maps"),
                  tr("Explore bundled maps without choosing a file provider."),
                  ActionIcon::fromName(u"symbols"));
  connect(examples_button, &QAbstractButton::clicked, this,
          &HomeScreenWidgetMobile::showExamples);
  content_layout->addWidget(make_action_group({examples_button}));

  auto *utility_card = new QFrame(content);
  utility_card->setObjectName(QStringLiteral("startupUtilityCard"));
  utility_card->setStyleSheet(QStringLiteral(
      "QFrame#startupUtilityCard { background: palette(base); border: 1px "
      "solid palette(midlight); border-radius: 14px; }"
      "QFrame#startupUtilityCard QPushButton { background: transparent; "
      "border: 0; padding: 10px 6px; }"
      "QFrame#startupUtilityCard QPushButton:pressed { background: "
      "palette(alternate-base); }"));
  auto *utility_buttons = new QHBoxLayout(utility_card);
  utility_buttons->setContentsMargins(4, 4, 4, 4);
  utility_buttons->setSpacing(0);
  auto *settings_button = new QPushButton(ActionIcon::fromName(u"settings"),
                                          tr("Settings"), content);
  auto *help_button =
      new QPushButton(ActionIcon::fromName(u"help"), tr("Help"), content);
  auto *about_button =
      new QPushButton(ActionIcon::fromName(u"about"), tr("About"), content);
  connect(settings_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showSettings);
  connect(help_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showHelp);
  connect(about_button, &QAbstractButton::clicked, controller->getWindow(),
          &MainWindow::showAbout);
  utility_buttons->addWidget(settings_button);
  utility_buttons->addWidget(help_button);
  utility_buttons->addWidget(about_button);
  content_layout->addWidget(utility_card);
  content_layout->addStretch();

  scroll_area->setWidget(content);
  auto *outer_layout = new QVBoxLayout(this);
  outer_layout->setContentsMargins({});
  outer_layout->addWidget(scroll_area);
  setAutoFillBackground(false);
#else
  auto *layout = new QVBoxLayout();
  layout->setSpacing(2 * layout->spacing());

  title_pixmap =
      QPixmap::fromImage(QImage(QString::fromLatin1(":/images/title.png")));
  title_label = new QLabel();
  title_label->setPixmap(title_pixmap);
  title_label->setAlignment(Qt::AlignCenter);
  layout->addWidget(title_label);

  auto *document_buttons = new QHBoxLayout();
  auto *new_button = new QPushButton(tr("Create new map"));
  auto *open_button = new QPushButton(tr("Open map"));
  auto *map_hub_button = new QPushButton(tr("Map Hub"));
  connect(new_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showNewMapWizard);
  connect(open_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showOpenDialog);
  connect(map_hub_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showMapHub);
  document_buttons->addWidget(new_button);
  document_buttons->addWidget(open_button);
  document_buttons->addWidget(map_hub_button);
  layout->addLayout(document_buttons);

  file_list_widget = makeFileListWidget();
  connect(file_list_widget, &QListWidget::itemClicked, this,
          &HomeScreenWidgetMobile::itemClicked);
  layout->addWidget(file_list_widget, 1);

  auto settings_button =
      new QPushButton(HomeScreenWidgetDesktop::tr("Settings"));
  connect(settings_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showSettings);
  QPushButton *help_button =
      new QPushButton(HomeScreenWidgetDesktop::tr("Help"));
  connect(help_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showHelp);
  QPushButton *about_button = new QPushButton(tr("About Mapper"));
  connect(about_button, &QPushButton::clicked, controller->getWindow(),
          &MainWindow::showAbout);
  QHBoxLayout *buttons_layout = new QHBoxLayout();
  buttons_layout->setContentsMargins(0, 0, 0, 0);
  buttons_layout->addWidget(settings_button);
  buttons_layout->addStretch(1);
  buttons_layout->addWidget(help_button);
  buttons_layout->addWidget(about_button);
  layout->addLayout(buttons_layout);

  setLayout(layout);
  setAutoFillBackground(false);

  updateFileListWidget();
#endif
}

HomeScreenWidgetMobile::~HomeScreenWidgetMobile() = default;

void HomeScreenWidgetMobile::resizeEvent(QResizeEvent * /*event*/) {
#if !defined(Q_OS_IOS)
  adjustTitlePixmapSize();
#endif
}

void HomeScreenWidgetMobile::adjustTitlePixmapSize() {
  auto label_size = title_label->size();
  auto scaled_width =
      qRound(title_pixmap.devicePixelRatio() * label_size.width());
  if (title_pixmap.width() > scaled_width) {
    if (title_label->pixmap().width() != scaled_width) {
      label_size.setHeight(title_pixmap.height());
      title_label->setPixmap(title_pixmap.scaled(
          label_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
  } else if (title_label->pixmap().width() != title_pixmap.width()) {
    title_label->setPixmap(title_pixmap);
  }
}

void HomeScreenWidgetMobile::setRecentFiles(const QStringList &files) {
#if defined(Q_OS_IOS)
  file_list_widget->clear();
  for (const auto &file_path : files) {
    if (!DocumentPath::isContentUri(file_path) && !QFileInfo::exists(file_path))
      continue;
    auto *item = new QListWidgetItem(ActionIcon::fromName(u"open"),
                                     DocumentPath::displayName(file_path));
    item->setData(pathRole(), file_path);
    item->setData(hintRole(), StorageLocation::HintNormal);
    item->setToolTip(file_path);
    file_list_widget->addItem(item);
  }
  const auto visible_rows = qMin(file_list_widget->count(), 4);
  const auto row_height = file_list_widget->count() > 0
                              ? file_list_widget->sizeHintForRow(0)
                              : fontMetrics().height() + 24;
  file_list_widget->setFixedHeight(qMax(1, visible_rows) * row_height + 2);
  file_list_widget->setVisible(file_list_widget->count() > 0);
  recent_files_empty_label->setVisible(file_list_widget->count() == 0);
#else
  Q_UNUSED(files)
  if (history.empty())
    updateFileListWidget();
#endif
}

void HomeScreenWidgetMobile::setOpenMRUFileChecked(bool /*state*/) {
  // nothing
}

void HomeScreenWidgetMobile::setTipOfTheDay(const QString & /*text*/) {
  // nothing
}

void HomeScreenWidgetMobile::setTipsVisible(bool /*state*/) {
  // nothing
}

void HomeScreenWidgetMobile::showSettings() {
  auto window = this->window();

  SettingsDialog dialog(window);
  dialog.setGeometry(window->geometry());
  dialog.exec();
}

void HomeScreenWidgetMobile::showGnssSettings() {
  auto *main_window = controller->getWindow();
  SettingsDialog dialog(main_window);
  dialog.selectPage(tr("GNSS"));
  dialog.exec();
#ifdef MAPPER_GNSS_AVAILABLE
  const auto &settings = Settings::getInstance();
  if (!gnss_button)
    return;
  if (settings.gnssDeviceName().isEmpty())
    gnss_button->setDescription(
        tr("Use iPhone location, or configure a Bluetooth GNSS receiver and "
           "NTRIP corrections."));
  else
    gnss_button->setDescription(
        settings.gnssAutoStartNtrip()
            ? tr("%1 with NTRIP corrections.").arg(settings.gnssDeviceName())
            : tr("%1 without automatic corrections.")
                  .arg(settings.gnssDeviceName()));
#endif
}

void HomeScreenWidgetMobile::showExamples() {
  QDialog dialog(controller->getWindow());
  dialog.setWindowTitle(tr("Example maps"));
  if (auto *main_window = controller->getWindow()) {
    dialog.setAttribute(Qt::WA_WindowPropagation);
    dialog.setPalette(main_window->palette());
    dialog.resize(main_window->size());
  }
  auto mobile_font = dialog.font();
  mobile_font.setPointSizeF(16.0);
  dialog.setFont(mobile_font);
  dialog.setStyleSheet(QStringLiteral(
      "QDialog { background: palette(window); }"
      "QListWidget { background: palette(base); border: 1px solid "
      "palette(midlight); border-radius: 14px; }"
      "QListWidget::item { padding: 13px 10px; border-bottom: 1px solid "
      "palette(midlight); }"
      "QPushButton#examplesQuiet { background: transparent; border: 0; "
      "color: palette(highlight); padding: 10px 14px; }"));

  auto *intro = new QLabel(tr("These read-only examples are included with "
                              "Mapper. Choose one to open it."),
                           &dialog);
  intro->setWordWrap(true);
  intro->setStyleSheet(QStringLiteral("color: palette(mid);"));
  auto *list = new QListWidget(&dialog);
  QScroller::grabGesture(list->viewport(), QScroller::TouchGesture);
  list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  const auto directory = QDir(QLatin1String("data:/examples"));
  const auto entries = directory.entryInfoList(
      QDir::Files | QDir::NoDotAndDotDot,
      QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
  for (const auto &file_info : entries) {
    const auto *format = FileFormats.findFormatForFilename(
        file_info.filePath(), &FileFormat::supportsReading);
    if (!format || format->fileType() != FileFormat::MapFile)
      continue;
    auto *item = new QListWidgetItem(ActionIcon::fromName(u"open"),
                                     file_info.completeBaseName(), list);
    item->setData(pathRole(), file_info.filePath());
  }
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Cancel)
      ->setObjectName(QStringLiteral("examplesQuiet"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(18, 18, 18, 14);
  layout->setSpacing(12);
  auto *title = new QLabel(tr("Example maps"), &dialog);
  auto title_font = title->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 1.8);
  title_font.setBold(true);
  title->setFont(title_font);
  layout->addWidget(title);
  layout->addWidget(intro);
  layout->addWidget(list, 1);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(list, &QListWidget::itemClicked, &dialog,
          [&dialog](QListWidgetItem *) { dialog.accept(); });
  if (dialog.exec() != QDialog::Accepted || !list->currentItem())
    return;
  controller->getWindow()->openPath(
      list->currentItem()->data(pathRole()).toString());
}

void HomeScreenWidgetMobile::itemClicked(QListWidgetItem *item) {
  auto file_path = item->data(pathRole()).toString();
  auto hint =
      static_cast<StorageLocation::Hint>(item->data(hintRole()).toInt());

  if (file_path == QLatin1String("doc:")) {
#ifdef Q_OS_ANDROID
    Util::showHelp(window(), "android-storage.html");
#endif
  } else if (file_path == QLatin1String("..")) {
    if (!history.empty())
      history.pop_back();
    updateFileListWidget();
  } else if (hint == StorageLocation::HintNoAccess) {
    QMessageBox::warning(
        this, ::OpenOrienteering::MainWindow::tr("Warning"),
        StorageLocation::fileHintTextTemplate(hint).arg(file_path));
  } else if (DocumentPath::isContentUri(file_path)) {
    setEnabled(false);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                100 /* ms */);
    controller->getWindow()->openPath(file_path);
    setEnabled(true);
  } else if (QFileInfo(file_path).isDir()) {
    history.emplace_back(file_path, hint);
    updateFileListWidget();
  } else {
    setEnabled(false);
    if (hint != StorageLocation::HintNormal) {
      auto hint_text = StorageLocation::fileHintTextTemplate(hint);
      QMessageBox::warning(
          this, ::OpenOrienteering::MainWindow::tr("Warning"),
          hint_text.arg(item->data(Qt::DisplayRole).toString()));
    }

    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents,
                                100 /* ms */);
    controller->getWindow()->openPath(file_path);
    setEnabled(true);
  }
}

QListWidget *HomeScreenWidgetMobile::makeFileListWidget() {
  file_list_widget = new QListWidget();
  QScroller::grabGesture(file_list_widget->viewport(), QScroller::TouchGesture);
  QFont list_font = file_list_widget->font();
  int pixel_size = list_font.pixelSize();
  if (pixel_size > 0) {
    list_font.setPixelSize(pixel_size * 3 / 2);
  } else {
    pixel_size = list_font.pointSize();
    list_font.setPointSize(pixel_size * 3 / 2);
  }
  file_list_widget->setFont(list_font);
  file_list_widget->setSpacing(pixel_size / 2);
  file_list_widget->setCursor(Qt::PointingHandCursor);
  file_list_widget->setStyleSheet(QString::fromLatin1(" \
	  QListWidget::item:hover { \
	    color: palette(highlighted-text); \
	    background: palette(highlight); \
	  } "));

  return file_list_widget;
}

void HomeScreenWidgetMobile::updateFileListWidget() {
  file_list_widget->clear();

  if (history.empty()) {
    // First screen.
    // Recent files first.
    Settings &settings = Settings::getInstance();
    auto recent_files =
        settings.getSetting(Settings::General_RecentFilesList).toStringList();
    for (auto &file_path : recent_files) {
      if (DocumentPath::isContentUri(file_path)) {
        auto *item = new QListWidgetItem(DocumentPath::displayName(file_path));
        item->setData(pathRole(), file_path);
        item->setData(hintRole(), StorageLocation::HintNormal);
        item->setToolTip(file_path);
        file_list_widget->addItem(item);
      } else {
        auto file_info = QFileInfo(file_path);
        if (file_info.exists())
          addItemToFileList(file_info);
      }
    }

#ifdef Q_OS_ANDROID
    // If there are no recent files, offer a link to the Android storage manual
    // page.
    if (file_list_widget->count() == 0) {
      auto *help_item = new QListWidgetItem(tr("Help"));
      help_item->setData(pathRole(), QLatin1String("doc:"));
      help_item->setIcon(
          file_list_widget->style()->standardIcon(QStyle::SP_DialogHelpButton));
      file_list_widget->addItem(help_item);
    }
#endif

    // Device-specific locations next.
    // For disambiguation, using the full path for the label.
    StorageLocation::refresh();
    const auto locations = StorageLocation::knownLocations();
    for (const auto &location : *locations) {
      auto file_info = QFileInfo(location.path());
      auto icon = file_list_widget->style()->standardIcon(QStyle::SP_DirIcon);
      addItemToFileList(location.path(), file_info, location.hint(), icon);
    }

    // Examples last.
    // The examples path isn't writable, so the hint will be overridden.
    auto file_info = QFileInfo(QLatin1String("data:/examples"));
    addItemToFileList(tr("Examples"), file_info);
  } else {
    // Backwards navigation on top.
    auto *parent_item = new QListWidgetItem(QLatin1String(".."));
    parent_item->setData(pathRole(), QLatin1String(".."));
    parent_item->setIcon(
        file_list_widget->style()->standardIcon(QStyle::SP_FileDialogToParent));
    file_list_widget->addItem(parent_item);

    // Contents of selected location, files first.
    const auto &location = history.back();
    QIcon icon;
    switch (location.hint()) {
    case StorageLocation::HintApplication:
      icon = file_list_widget->style()->standardIcon(
          QStyle::SP_MessageBoxInformation);
      break;
    case StorageLocation::HintReadOnly:
      icon =
          file_list_widget->style()->standardIcon(QStyle::SP_MessageBoxWarning);
      break;
    case StorageLocation::HintNormal:
    case StorageLocation::HintNoAccess:
    case StorageLocation::HintInvalid:
      break;
    }

    constexpr auto filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot;
    constexpr auto flags =
        QDir::DirsLast | QDir::Name | QDir::IgnoreCase | QDir::LocaleAware;
    auto const info_list = QDir(location.path()).entryInfoList(filters, flags);
    for (const auto &file_info : info_list) {
      addItemToFileList(file_info, location.hint(), icon);
    }
  }
}

void HomeScreenWidgetMobile::addItemToFileList(const QFileInfo &file_info,
                                               int hint, const QIcon &icon) {
  addItemToFileList(file_info.fileName(), file_info, hint, icon);
}

void HomeScreenWidgetMobile::addItemToFileList(const QString &label,
                                               const QFileInfo &file_info,
                                               int hint, const QIcon &icon) {
  const auto file_path = file_info.filePath();
  if (hint == StorageLocation::HintNoAccess) {
    // When there is no access, avoid extra QFileInfo calls.
    auto *new_item = new QListWidgetItem(label);
    new_item->setData(pathRole(), file_path);
    new_item->setData(hintRole(), hint);
    new_item->setToolTip(
        StorageLocation::fileHintTextTemplate(StorageLocation::HintNoAccess)
            .arg(file_path));
    new_item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxQuestion));
    file_list_widget->addItem(new_item);
    return;
  }

  const auto *format = FileFormats.findFormatForFilename(
      file_path, &FileFormat::supportsReading);
  if (file_info.isDir() ||
      (format && format->fileType() == FileFormat::MapFile)) {
    auto *new_item = new QListWidgetItem(label);
    new_item->setData(pathRole(), file_path);
    new_item->setData(hintRole(), hint);
    new_item->setToolTip(file_path);
    if (file_info.isDir()) {
      // Use dir icon.
      new_item->setIcon(icon.isNull() ? file_list_widget->style()->standardIcon(
                                            QStyle::SP_DirIcon)
                                      : icon);
    } else if (hint == StorageLocation::HintReadOnly ||
               (file_info.isWritable() && format->supportsWriting())) {
      // Use icon as-is.
      new_item->setIcon(icon);
    } else {
      // Override with read-only warning.
      new_item->setData(hintRole(), StorageLocation::HintReadOnly);
      new_item->setIcon(file_list_widget->style()->standardIcon(
          QStyle::SP_MessageBoxWarning));
      new_item->setToolTip(
          StorageLocation::fileHintTextTemplate(StorageLocation::HintReadOnly)
              .arg(file_path));
    }
    file_list_widget->addItem(new_item);
  }
}

} // namespace OpenOrienteering
