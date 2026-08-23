#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QSpinBox;

class OscSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit OscSettingsDialog(QWidget *parent = nullptr);

protected:
	void accept(void) override;

private:
	QSpinBox *port_;
	QCheckBox *feedback_enabled_;
	QLineEdit *feedback_host_;
	QSpinBox *feedback_port_;
	QCheckBox *query_enabled_;
	QSpinBox *query_port_;
};
