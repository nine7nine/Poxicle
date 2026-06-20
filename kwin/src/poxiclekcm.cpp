/* poxiclekcm.cpp — see poxiclekcm.h. */
#include "poxiclekcm.h"

#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

namespace {

// Preset names: order shown in the combo. Stored verbatim in the rule.
const QStringList kPresets = {
    QStringLiteral("ambient"), QStringLiteral("none"), QStringLiteral("corners"),
    QStringLiteral("fireworks"), QStringLiteral("ping-pong"), QStringLiteral("pulse-out"),
    QStringLiteral("rotate"), QStringLiteral("laser"), QStringLiteral("tracer"),
    QStringLiteral("comet"), QStringLiteral("spinner"), QStringLiteral("ripple"),
    QStringLiteral("charge"),
};
// Index 0 == inherit ("—") for the sentinel combos.
const QStringList kShapes   = { QStringLiteral("—"), QStringLiteral("Square"),
                                QStringLiteral("Circle"), QStringLiteral("Diamond"),
                                QStringLiteral("Triangle") };
const QStringList kGaps     = { QStringLiteral("—"), QStringLiteral("Solid"),
                                QStringLiteral("Gapped") };
const QStringList kReverse  = { QStringLiteral("▶"), QStringLiteral("◀") };
const QStringList kRelease  = { QStringLiteral("—"), QStringLiteral("Uniform"),
                                QStringLiteral("Retract"), QStringLiteral("Spread"),
                                QStringLiteral("Grow"), QStringLiteral("All") };

// Columns, in App Glass order (glass color dropped).
enum Col {
    ColApp = 0, ColPreset, ColRev, ColColor, ColShape, ColGap,
    ColSpd, ColThk, ColTail, ColAtk, ColRel, ColRls, ColTAtk, ColTRel, ColTRls,
    ColCount
};

const char *kColorProp = "poxColor";   // stores the chosen "#rrggbb" on the button

QComboBox *combo(const QStringList &items, int current)
{
    auto *c = new QComboBox;
    c->addItems(items);
    c->setCurrentIndex(qBound(0, current, items.size() - 1));
    return c;
}

QSpinBox *spin(int max, int value)
{
    auto *s = new QSpinBox;
    s->setRange(0, max);
    s->setSpecialValueText(QStringLiteral("—"));  // 0 shows as inherit
    s->setValue(value);
    return s;
}

int asInt(const QString &s, int dflt)
{
    bool ok = false;
    const int v = s.toInt(&ok);
    return ok ? v : dflt;
}

} // namespace

PoxicleKcm::PoxicleKcm(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    auto *root = new QVBoxLayout(widget());

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(i18n("New app preset:")));
    m_defaultPreset = combo(kPresets, 0);
    connect(m_defaultPreset, &QComboBox::currentIndexChanged, this, [this] { setNeedsSave(true); });
    top->addWidget(m_defaultPreset);
    top->addStretch();
    auto *addBtn = new QPushButton(i18n("Add app"));
    auto *delBtn = new QPushButton(i18n("Remove selected"));
    top->addWidget(addBtn);
    top->addWidget(delBtn);
    root->addLayout(top);

    m_table = new QTableWidget(0, ColCount, widget());
    m_table->setHorizontalHeaderLabels({
        i18n("App"), i18n("Preset"), i18n("Rev"), i18n("Color"), i18n("Shape"),
        i18n("Gap"), i18n("Spd"), i18n("Thk"), i18n("Tail"), i18n("Atk"),
        i18n("Rel"), i18n("Rls"), i18n("TAtk"), i18n("TRel"), i18n("TRls"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(ColApp, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(m_table);

    auto *hint = new QLabel(i18n(
        "Only the apps listed here get particles. Match is a case-insensitive "
        "substring of the window's app id (windowClass). A preset of “none” turns "
        "an app off; “—” / 0 in a cell means inherit from the preset."));
    hint->setWordWrap(true);
    root->addWidget(hint);

    connect(addBtn, &QPushButton::clicked, this, [this] {
        addRow(QStringLiteral("|") + m_defaultPreset->currentText());  // empty app, chosen preset
        setNeedsSave(true);
    });
    connect(delBtn, &QPushButton::clicked, this, [this] {
        const int r = m_table->currentRow();
        if (r >= 0) { m_table->removeRow(r); setNeedsSave(true); }
    });
}

void PoxicleKcm::addRow(const QString &packedRule)
{
    const QStringList f = packedRule.split(QLatin1Char('|'));
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    m_table->setItem(row, ColApp, new QTableWidgetItem(f.value(0)));

    auto *preset = combo(kPresets, qMax(0, kPresets.indexOf(f.value(1, QStringLiteral("ambient")))));
    auto *rev    = combo(kReverse, asInt(f.value(2), 0));
    auto *shape  = combo(kShapes,  asInt(f.value(4), -1) + 1);   // -1 -> index 0
    auto *gap    = combo(kGaps,    asInt(f.value(5), -1) + 1);
    auto *spd    = spin(300, asInt(f.value(6),  0));
    auto *thk    = spin(40,  asInt(f.value(7),  0));
    auto *tail   = spin(300, asInt(f.value(8),  0));
    auto *atk    = spin(50,  asInt(f.value(9),  0));
    auto *rel    = spin(50,  asInt(f.value(10), 0));
    auto *rls    = combo(kRelease, asInt(f.value(11), -1) + 1);
    auto *tatk   = spin(50,  asInt(f.value(12), 0));
    auto *trel   = spin(50,  asInt(f.value(13), 0));
    auto *trls   = combo(kRelease, asInt(f.value(14), -1) + 1);

    auto *colorBtn = new QPushButton;
    const QString pc = f.value(3);
    colorBtn->setProperty(kColorProp, pc);
    colorBtn->setText(pc.isEmpty() ? i18n("—") : pc);
    if (!pc.isEmpty())
        colorBtn->setStyleSheet(QStringLiteral("background:%1;").arg(pc));
    connect(colorBtn, &QPushButton::clicked, this, [this, row] { chooseColor(row); });

    m_table->setCellWidget(row, ColPreset, preset);
    m_table->setCellWidget(row, ColRev,    rev);
    m_table->setCellWidget(row, ColColor,  colorBtn);
    m_table->setCellWidget(row, ColShape,  shape);
    m_table->setCellWidget(row, ColGap,    gap);
    m_table->setCellWidget(row, ColSpd,    spd);
    m_table->setCellWidget(row, ColThk,    thk);
    m_table->setCellWidget(row, ColTail,   tail);
    m_table->setCellWidget(row, ColAtk,    atk);
    m_table->setCellWidget(row, ColRel,    rel);
    m_table->setCellWidget(row, ColRls,    rls);
    m_table->setCellWidget(row, ColTAtk,   tatk);
    m_table->setCellWidget(row, ColTRel,   trel);
    m_table->setCellWidget(row, ColTRls,   trls);

    // Any change marks the page dirty.
    for (QComboBox *c : { preset, rev, shape, gap, rls, trls })
        connect(c, &QComboBox::currentIndexChanged, this, [this] { setNeedsSave(true); });
    for (QSpinBox *s : { spd, thk, tail, atk, rel, tatk, trel })
        connect(s, &QSpinBox::valueChanged, this, [this] { setNeedsSave(true); });
    connect(m_table, &QTableWidget::cellChanged, this, [this] { setNeedsSave(true); });
}

void PoxicleKcm::chooseColor(int row)
{
    auto *btn = qobject_cast<QPushButton *>(m_table->cellWidget(row, ColColor));
    if (!btn)
        return;
    const QColor initial(btn->property(kColorProp).toString());
    const QColor c = QColorDialog::getColor(initial.isValid() ? initial : QColor(Qt::white),
                                            widget(), i18n("Particle color"));
    if (!c.isValid())
        return;
    const QString hex = c.name(QColor::HexRgb);
    btn->setProperty(kColorProp, hex);
    btn->setText(hex);
    btn->setStyleSheet(QStringLiteral("background:%1;").arg(hex));
    setNeedsSave(true);
}

QString PoxicleKcm::rowToRule(int row) const
{
    const QTableWidgetItem *app = m_table->item(row, ColApp);
    const QString appId = app ? app->text().trimmed() : QString();
    if (appId.isEmpty())
        return QString();

    const auto cb = [this, row](int col) {
        return qobject_cast<QComboBox *>(m_table->cellWidget(row, col));
    };
    const auto sb = [this, row](int col) {
        auto *s = qobject_cast<QSpinBox *>(m_table->cellWidget(row, col));
        return s ? s->value() : 0;
    };
    auto *colorBtn = qobject_cast<QPushButton *>(m_table->cellWidget(row, ColColor));

    const QString preset = cb(ColPreset) ? cb(ColPreset)->currentText() : QStringLiteral("ambient");
    const int rev   = cb(ColRev)   ? cb(ColRev)->currentIndex() : 0;
    const int shape = cb(ColShape) ? cb(ColShape)->currentIndex() - 1 : -1;   // index 0 -> -1
    const int gap   = cb(ColGap)   ? cb(ColGap)->currentIndex() - 1 : -1;
    const int rls   = cb(ColRls)   ? cb(ColRls)->currentIndex() - 1 : -1;
    const int trls  = cb(ColTRls)  ? cb(ColTRls)->currentIndex() - 1 : -1;
    const QString color = colorBtn ? colorBtn->property(kColorProp).toString() : QString();

    return QStringList{
        appId, preset, QString::number(rev), color,
        QString::number(shape), QString::number(gap),
        QString::number(sb(ColSpd)), QString::number(sb(ColThk)), QString::number(sb(ColTail)),
        QString::number(sb(ColAtk)), QString::number(sb(ColRel)), QString::number(rls),
        QString::number(sb(ColTAtk)), QString::number(sb(ColTRel)), QString::number(trls),
    }.join(QLatin1Char('|'));
}

void PoxicleKcm::load()
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                         ->group(QStringLiteral("Effect-poxicle_kwin"));

    const QString def = g.readEntry("DefaultPreset", QStringLiteral("ambient"));
    m_defaultPreset->setCurrentIndex(qMax(0, kPresets.indexOf(def)));

    m_table->setRowCount(0);
    const QStringList rules = g.readEntry("Rules", QStringList());
    for (const QString &r : rules)
        addRow(r);

    setNeedsSave(false);
}

void PoxicleKcm::save()
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                         ->group(QStringLiteral("Effect-poxicle_kwin"));

    g.writeEntry("DefaultPreset", m_defaultPreset->currentText());

    QStringList rules;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString rule = rowToRule(r);
        if (!rule.isEmpty())
            rules << rule;
    }
    g.writeEntry("Rules", rules);
    g.sync();

    // Ask KWin to reload our effect so changes apply live.
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"),
        QStringLiteral("org.kde.kwin.Effects"), QStringLiteral("reconfigureEffect"));
    msg << QStringLiteral("poxicle_kwin");
    QDBusConnection::sessionBus().send(msg);

    setNeedsSave(false);
}

void PoxicleKcm::defaults()
{
    m_defaultPreset->setCurrentIndex(qMax(0, kPresets.indexOf(QStringLiteral("ambient"))));
    m_table->setRowCount(0);
    setNeedsSave(true);
}

#include <KPluginFactory>
K_PLUGIN_CLASS_WITH_JSON(PoxicleKcm, "kcm-poxicle.json")

#include "poxiclekcm.moc"
