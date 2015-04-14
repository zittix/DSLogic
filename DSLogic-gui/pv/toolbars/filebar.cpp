/*
 * This file is part of the DSLogic-gui project.
 * DSLogic-gui is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <dreamsourcelab@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/shared_ptr.hpp>

#include <QMetaObject>
#include <QMessageBox>
#include <QFileDialog>
#include <QApplication>

#include "filebar.h"
#include "../device/devinst.h"

#include <deque>

namespace pv {
namespace toolbars {

FileBar::FileBar(SigSession &session, QWidget *parent) :
    QToolBar("File Bar", parent),
    _enable(true),
    _session(session),
    _file_button(this)
{
    setMovable(false);

    _action_open = new QAction(this);
    _action_open->setText(QApplication::translate(
        "File", "&Open...", 0, QApplication::UnicodeUTF8));
    _action_open->setIcon(QIcon::fromTheme("file",
        QIcon(":/icons/open.png")));
    _action_open->setObjectName(QString::fromUtf8("actionOpen"));
    _file_button.addAction(_action_open);
    connect(_action_open, SIGNAL(triggered()), this, SLOT(on_actionOpen_triggered()));

    _action_save = new QAction(this);
    _action_save->setText(QApplication::translate(
        "File", "&Save...", 0, QApplication::UnicodeUTF8));
    _action_save->setIcon(QIcon::fromTheme("file",
        QIcon(":/icons/save.png")));
    _action_save->setObjectName(QString::fromUtf8("actionSave"));
    _file_button.addAction(_action_save);
    connect(_action_save, SIGNAL(triggered()), this, SLOT(on_actionSave_triggered()));

    _action_capture = new QAction(this);
    _action_capture->setText(QApplication::translate(
        "File", "&Capture...", 0, QApplication::UnicodeUTF8));
    _action_capture->setIcon(QIcon::fromTheme("file",
        QIcon(":/icons/capture.png")));
    _action_capture->setObjectName(QString::fromUtf8("actionCapture"));
    _file_button.addAction(_action_capture);
    connect(_action_capture, SIGNAL(triggered()), this, SLOT(on_actionCapture_triggered()));

    _file_button.setPopupMode(QToolButton::InstantPopup);
    _file_button.setIcon(QIcon(":/icons/file.png"));

    addWidget(&_file_button);
}

void FileBar::on_actionOpen_triggered()
{
    // Show the dialog
    const QString file_name = QFileDialog::getOpenFileName(
        this, tr("Open File"), "", tr(
            "DSLogic Sessions (*.dsl)"));
    if (!file_name.isEmpty())
        load_file(file_name);
}

void FileBar::session_error(
    const QString text, const QString info_text)
{
    QMetaObject::invokeMethod(this, "show_session_error",
        Qt::QueuedConnection, Q_ARG(QString, text),
        Q_ARG(QString, info_text));
}

void FileBar::show_session_error(
    const QString text, const QString info_text)
{
    QMessageBox msg(this);
    msg.setText(text);
    msg.setInformativeText(info_text);
    msg.setStandardButtons(QMessageBox::Ok);
    msg.setIcon(QMessageBox::Warning);
    msg.exec();
}

void FileBar::on_actionSave_triggered()
{
    //save();
    int unit_size;
    uint64_t length;
    void* buf = _session.get_buf(unit_size, length);
    if (!buf) {
        QMessageBox msg(this);
        msg.setText("File Save");
        msg.setInformativeText("No Data to Save!");
        msg.setStandardButtons(QMessageBox::Ok);
        msg.setIcon(QMessageBox::Warning);
        msg.exec();
    } else if (_session.get_device()->dev_inst()->mode != LOGIC) {
        const QString file_name = QFileDialog::getSaveFileName(
                    this, tr("Save CSV File"), "",
                    tr("DSLogic Session CSV (*.csv)"));
        if (!file_name.isEmpty()) {
            _session.save_file(file_name.toStdString());
        }
    } else {
        const QString file_name = QFileDialog::getSaveFileName(
                    this, tr("Save File"), "",
                    tr("DSLogic Session (*.dsl)"));
        if (!file_name.isEmpty()) {
            _session.save_file(file_name.toStdString());
        }
    }
}

void FileBar::on_actionCapture_triggered()
{
    on_screenShot();
}

void FileBar::enable_toggle(bool enable)
{
    _file_button.setDisabled(!enable);
    _file_button.setIcon(enable ? QIcon(":/icons/file.png") :
                                  QIcon(":/icons/file_dis.png"));
}

} // namespace toolbars
} // namespace pv
