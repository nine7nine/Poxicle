/* poxiclekcm.h
 *
 * The "Configure…" KCM for the Poxicle KWin effect: an App Glass-style per-app
 * rules grid, reading/writing the exact kwinrc [Effect-poxicle_kwin] format the
 * effect consumes (see poxconfig.{h,cpp}). Plain QWidgets so it integrates as a
 * binary KCModule loaded from kwin/effects/configs/.
 */
#pragma once

#include <KCModule>

class QTableWidget;
class QComboBox;
class KPluginMetaData;

class PoxicleKcm : public KCModule
{
    Q_OBJECT

public:
    explicit PoxicleKcm(QObject *parent, const KPluginMetaData &data);

    void load() override;
    void save() override;
    void defaults() override;

private:
    void    addRow(const QString &packedRule);  // empty => a blank default row
    QString rowToRule(int row) const;            // serialize a row to the packed format
    void    chooseColor(int row);

    QTableWidget *m_table = nullptr;
    QComboBox    *m_defaultPreset = nullptr;
};
