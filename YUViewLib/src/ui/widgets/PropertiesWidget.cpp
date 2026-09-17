/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "PropertiesWidget.h"

#include "playlistitem/playlistItem.h"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QScrollArea>
#include <QWheelEvent>

/* The file info group box can display information on a file (or any other display object).
 * If you provide a list of QString tuples, this class will fill a grid layout with the
 * corresponding labels.
 */
PropertiesWidget::PropertiesWidget(QWidget *parent) : QWidget(parent), topLayout(this)
{
  topLayout.setContentsMargins(0, 0, 0, 0);
  topLayout.addWidget(&stack);

  // Create and add the empty widget. This widget is shown when no item is selected.
  stack.addWidget(&emptyWidget);
  stack.setCurrentWidget(&emptyWidget);
}

void PropertiesWidget::currentSelectedItemsChanged(playlistItem *item1, playlistItem *)
{
  // The properties are always just shown for the first item
  if (parentWidget())
  {
    if (item1)
      parentWidget()->setWindowTitle(item1->properties().propertiesWidgetTitle);
    else
      parentWidget()->setWindowTitle(PROPERTIESWIDGET_DEFAULT_WINDOW_TITLE);
  }

  if (item1)
  {
    // Show the properties widget of the first selection
    QWidget *propertiesWidget = item1->getPropertiesWidget();

    // Check if we already have a scroll area wrapping this properties widget
    QScrollArea *existingScroll = nullptr;
    for (int i = 0; i < stack.count(); ++i)
    {
      auto *sa = qobject_cast<QScrollArea *>(stack.widget(i));
      if (sa && sa->widget() == propertiesWidget)
      {
        existingScroll = sa;
        break;
      }
    }

    if (!existingScroll)
    {
      // First time: wrap the properties widget in a scroll area
      auto *scrollArea = new QScrollArea;
      scrollArea->setWidget(propertiesWidget);
      scrollArea->setWidgetResizable(true);
      scrollArea->setFrameShape(QFrame::NoFrame);
      stack.addWidget(scrollArea);
      existingScroll = scrollArea;
    }

    // Keep wheel scrolling on the properties panel instead of changing the
    // value of a combo box or spin box under the mouse pointer.
    installWheelEventFilters(propertiesWidget);

    stack.setCurrentWidget(existingScroll);
  }
  else
  {
    // Show the empty widget
    stack.setCurrentWidget(&emptyWidget);
  }
}

void PropertiesWidget::installWheelEventFilters(QWidget *propertiesWidget)
{
  const auto comboBoxes = propertiesWidget->findChildren<QComboBox *>();
  for (auto *comboBox : comboBoxes)
    comboBox->installEventFilter(this);

  const auto spinBoxes = propertiesWidget->findChildren<QAbstractSpinBox *>();
  for (auto *spinBox : spinBoxes)
    spinBox->installEventFilter(this);
}

bool PropertiesWidget::eventFilter(QObject *watched, QEvent *event)
{
  if (event->type() != QEvent::Wheel ||
      (!qobject_cast<QComboBox *>(watched) && !qobject_cast<QAbstractSpinBox *>(watched)))
    return QWidget::eventFilter(watched, event);

  auto *widget = qobject_cast<QWidget *>(watched);
  for (QWidget *parent = widget ? widget->parentWidget() : nullptr; parent; parent = parent->parentWidget())
  {
    if (auto *scrollArea = qobject_cast<QScrollArea *>(parent))
    {
      // Forward the original event synchronously. QAbstractScrollArea only
      // needs its deltas/modifiers, so the control-local position is irrelevant.
      QCoreApplication::sendEvent(scrollArea->viewport(), event);
      return true;
    }
  }

  return QWidget::eventFilter(watched, event);
}

void PropertiesWidget::itemAboutToBeDeleted(playlistItem *item)
{
  if (item->propertiesWidgetCreated())
  {
    QWidget *w = item->getPropertiesWidget();
    // Find the scroll area that wraps this properties widget
    for (int i = 0; i < stack.count(); ++i)
    {
      auto *sa = qobject_cast<QScrollArea *>(stack.widget(i));
      if (sa && sa->widget() == w)
      {
        sa->takeWidget(); // Release ownership so playlistItem can delete it
        stack.removeWidget(sa);
        delete sa;
        break;
      }
    }
  }
}
