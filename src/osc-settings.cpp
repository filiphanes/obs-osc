#include "osc-settings.h"

#include "osc-plugin.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

static QString loc(const char *lookup)
{
	const char *text = nullptr;
	if (!obs_module_get_string(lookup, &text))
		text = lookup;
	return QString::fromUtf8(text);
}

OscSettingsDialog::OscSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(loc("DialogTitle"));

	port_ = new QSpinBox(this);
	port_->setRange(1, 65535);
	port_->setValue(g_config.port);

	feedback_enabled_ = new QCheckBox(loc("Feedback"), this);
	feedback_enabled_->setChecked(g_config.feedback_enabled);

	feedback_host_ = new QLineEdit(QString::fromStdString(g_config.feedback_host), this);
	feedback_host_->setPlaceholderText(loc("FeedbackHostHint"));

	feedback_port_ = new QSpinBox(this);
	feedback_port_->setRange(0, 65535);
	feedback_port_->setSpecialValueText(loc("FeedbackPortAuto"));
	feedback_port_->setValue(g_config.feedback_port);

	query_enabled_ = new QCheckBox(loc("Query"), this);
	query_enabled_->setChecked(g_config.query_enabled);

	query_port_ = new QSpinBox(this);
	query_port_->setRange(0, 65535);
	query_port_->setSpecialValueText(loc("QueryPortAuto"));
	query_port_->setValue(g_config.query_port);

	auto *form = new QFormLayout;
	form->addRow(loc("Port"), port_);
	form->addRow(QString(), feedback_enabled_);
	form->addRow(loc("FeedbackHost"), feedback_host_);
	form->addRow(loc("FeedbackPort"), feedback_port_);
	form->addRow(QString(), query_enabled_);
	form->addRow(loc("QueryPort"), query_port_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &OscSettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);
}

void OscSettingsDialog::accept(void)
{
	osc_config next;
	next.port = port_->value();
	next.feedback_enabled = feedback_enabled_->isChecked();
	next.feedback_host = feedback_host_->text().toStdString();
	next.feedback_port = feedback_port_->value();
	next.query_enabled = query_enabled_->isChecked();
	next.query_port = query_port_->value();

	/* All restart/rollback/persistence rules live in
	 * osc_apply_settings(); the dialog only reports failures. */
	switch (osc_apply_settings(next)) {
	case osc_apply_result::ok:
		QDialog::accept();
		return;
	case osc_apply_result::udp_port_failed:
		QMessageBox::warning(
			this, loc("DialogTitle"),
			loc("BindError").arg(QString::number(next.port), QString::number(g_config.port)));
		break;
	case osc_apply_result::query_port_failed:
		QMessageBox::warning(this, loc("DialogTitle"),
				     loc("QueryBindError").arg(QString::number(next.query_port)));
		break;
	}
}

void osc_show_settings_dialog(void)
{
	OscSettingsDialog dialog((QWidget *)obs_frontend_get_main_window());
	dialog.exec();
}
