// The Additional data page: LRIT additional-data files, grouped by service and type.
#include "mainwindow.h"
#include "mainwindow_common.h"

namespace {
// ---- additional data, as the GK-2A LRIT Mission Specification (NMSC, v1.0) lays it out --------
//
// File name (Table 4.15):  ADD_<data type, max 8>_<sequence NNN>_<YYYYMMDD>_<hhmmss>_<segment NN>.lrit
// Services (section 3.1.2): alphanumeric text (ANT: operation information, observation and
// dissemination schedules), sea information (sea surface information, weather charts, sea weather
// text) and GOCI-II data.
// Header records of an additional-data file (Table 4.16): 0 primary, 4 annotation, 5 time stamp,
// 7 key header. The data field follows the header records.

struct AddName {
    bool ok = false;
    QString type, sequence, segment;
    QDateTime disseminated;   // UTC
};

AddName parseAddName(const QString& fileName) {
    static const QRegularExpression re(
        QStringLiteral("^ADD_([A-Za-z0-9]{1,8})_(\\d+)_(\\d{8})_(\\d{6})_(\\d+)\\.lrit"),
        QRegularExpression::CaseInsensitiveOption);
    AddName n;
    const auto m = re.match(fileName);
    if (!m.hasMatch()) return n;
    n.ok = true;
    n.type = m.captured(1).toUpper();
    n.sequence = m.captured(2);
    n.segment = m.captured(5);
    n.disseminated = QDateTime::fromString(m.captured(3) + m.captured(4), QStringLiteral("yyyyMMddHHmmss"));
    n.disseminated.setTimeZone(QTimeZone(QTimeZone::UTC));
    return n;
}

enum class AddService { Text, Sea, Goci, Unidentified };

// The service a data type belongs to. ANT is the alphanumeric-text type the specification names;
// GOCI-II types are recognised by name; every other named type is one of the sea-information
// products (the specification lists no other service). A file without an ADD_ name is not guessed.
AddService addServiceOf(const QString& type) {
    if (type.isEmpty()) return AddService::Unidentified;
    if (type == QStringLiteral("ANT")) return AddService::Text;
    if (type.startsWith(QStringLiteral("GOCI"))) return AddService::Goci;
    return AddService::Sea;
}

QString addServiceTitle(AddService s) {
    switch (s) {
        case AddService::Text: return QStringLiteral("Alphanumeric text");
        case AddService::Sea: return QStringLiteral("Sea information / weather charts");
        case AddService::Goci: return QStringLiteral("GOCI-II");
        default: return QStringLiteral("Unidentified");
    }
}

QString addServiceNote(AddService s) {
    switch (s) {
        case AddService::Text: return QStringLiteral("ANT: operation information, observation and dissemination schedules");
        case AddService::Sea: return QStringLiteral("Sea surface information, weather charts, sea weather text (every named type other than ANT / GOCI)");
        case AddService::Goci: return QStringLiteral("Ocean colour data from GK-2B");
        default: return QStringLiteral("No ADD_ file name: the type cannot be read from the transmission");
    }
}

QString recordTypeName(int type) {
    switch (type) {
        case 0: return QStringLiteral("Primary header");
        case 1: return QStringLiteral("Image structure");
        case 2: return QStringLiteral("Image navigation");
        case 3: return QStringLiteral("Image data function");
        case 4: return QStringLiteral("Annotation");
        case 5: return QStringLiteral("Time stamp");
        case 6: return QStringLiteral("Ancillary text");
        case 7: return QStringLiteral("Key header");
        case 128: return QStringLiteral("Image segment identification");
        case 129: return QStringLiteral("Encryption key message header");
        case 130: return QStringLiteral("Image compensation information");
        case 131: return QStringLiteral("Image observation time");
        case 132: return QStringLiteral("Image quality information");
        default: return QStringLiteral("Record type %1").arg(type);
    }
}

const QStringList& addContentSuffixes() {
    static const QStringList s{QStringLiteral(".txt"), QStringLiteral(".gif"), QStringLiteral(".png"),
                               QStringLiteral(".jpg"), QStringLiteral(".bin")};
    return s;
}

// The transmission (.lrit) and the content the decoder wrote beside it (.lrit.<ext>), either of
// which may be missing.
struct AddFilePair { QString lrit, content; };

AddFilePair addFilePair(const QString& path) {
    AddFilePair p;
    const int at = path.lastIndexOf(QStringLiteral(".lrit."), -1, Qt::CaseInsensitive);
    if (path.endsWith(QStringLiteral(".lrit"), Qt::CaseInsensitive)) {
        p.lrit = path;
        for (const QString& s : addContentSuffixes())
            if (QFileInfo::exists(path + s)) { p.content = path + s; break; }
    } else if (at >= 0) {
        p.content = path;
        const QString raw = path.left(at + 5);
        if (QFileInfo::exists(raw)) p.lrit = raw;
    } else {
        p.content = path;
    }
    return p;
}

QString contentFormat(const QString& contentPath, const std::vector<std::uint8_t>& bytes) {
    const QString lower = contentPath.toLower();
    if (lower.endsWith(QStringLiteral(".txt"))) return QStringLiteral("Text");
    if (lower.endsWith(QStringLiteral(".gif"))) return QStringLiteral("GIF");
    if (lower.endsWith(QStringLiteral(".png"))) return QStringLiteral("PNG");
    if (lower.endsWith(QStringLiteral(".jpg"))) return QStringLiteral("JPEG");
    if (bytes.size() >= 4) {
        if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') return QStringLiteral("GIF");
        if (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') return QStringLiteral("PNG");
        if (bytes[0] == 0xFF && bytes[1] == 0xD8) return QStringLiteral("JPEG");
    }
    return QStringLiteral("Binary");
}

QString formatSize(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 KB").arg((bytes + 1023) / 1024);
}

QTreeWidgetItem* structureSection(QTreeWidget* tree, const QString& title, const QString& value = QString()) {
    auto* item = new QTreeWidgetItem(tree, {title, value});
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    item->setFirstColumnSpanned(value.isEmpty());
    item->setExpanded(true);
    return item;
}

void structureField(QTreeWidgetItem* parent, const QString& key, const QString& value) {
    auto* item = new QTreeWidgetItem(parent, {key, value});
    item->setToolTip(1, value);
}

std::uint32_t beU32(const std::vector<std::uint8_t>& b, std::size_t i) {
    return (std::uint32_t(b[i]) << 24) | (std::uint32_t(b[i + 1]) << 16) | (std::uint32_t(b[i + 2]) << 8) | b[i + 3];
}
}  // namespace

QWidget* MainWindow::makeAddPage() {
    // Laid out the way the additional-data service is defined: the list is grouped by service and
    // data type (from the file name), and a selected file is shown as its parts -- the file name
    // fields, the header records, and the data field itself.
    using namespace nadirstation::ui;
    auto* page = new QWidget;
    auto* root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    Card leftCard = card(QStringLiteral("Additional data files"));
    auto* left = leftCard.frame;
    auto* ll = leftCard.body;
    auto* dirRow = new QHBoxLayout;
    addDirEdit_ = new QLineEdit;
    addDirEdit_->setPlaceholderText(QStringLiteral("Folder to search"));
    dirRow->addWidget(addDirEdit_, 1);
    addBrowse_ = new QPushButton(QStringLiteral("Browse…"));
    dirRow->addWidget(addBrowse_);
    addRefresh_ = new QPushButton(QStringLiteral("Rescan"));
    dirRow->addWidget(addRefresh_);
    ll->addLayout(dirRow);

    addFileTree_ = new QTreeWidget;
    addFileTree_->setColumnCount(2);
    addFileTree_->setHeaderLabels({QStringLiteral("Service / type / file"), QStringLiteral("Data field")});
    addFileTree_->setRootIsDecorated(true);
    addFileTree_->setUniformRowHeights(true);
    addFileTree_->header()->setStretchLastSection(false);
    addFileTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    addFileTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ll->addWidget(addFileTree_, 1);
    auto* listNote = new QLabel(QStringLiteral("Files under folders named ADD, filed as <spacecraft>/ADD/<data type>/.\n"
                                               "Name format: ADD_<type>_<seq>_<YYYYMMDD>_<hhmmss>_<segment>.lrit"));
    listNote->setObjectName(QStringLiteral("Note"));
    listNote->setWordWrap(true);
    ll->addWidget(listNote);
    root->addWidget(left, 30);

    Card rightCard = card();
    auto* right = rightCard.frame;
    auto* rl = rightCard.body;
    addSummary_ = new QLabel(QStringLiteral("Select a file to read its additional data."));
    addSummary_->setWordWrap(true);
    addSummary_->setObjectName(QStringLiteral("CardTitle"));
    rl->addWidget(addSummary_);

    auto* split = new QSplitter(Qt::Horizontal);
    auto* dataBox = new QWidget;
    auto* dl = new QVBoxLayout(dataBox);
    dl->setContentsMargins(0, 0, 0, 0);
    auto* dataTitle = new QLabel(QStringLiteral("Data field"));
    dataTitle->setObjectName(QStringLiteral("SectionTitle"));
    dl->addWidget(dataTitle);
    addContent_ = new QPlainTextEdit;
    addContent_->setReadOnly(true);
    addContent_->setLineWrapMode(QPlainTextEdit::NoWrap);
    addContent_->setObjectName(QStringLiteral("LogView"));
    addContent_->setPlaceholderText(QStringLiteral("Select a file to read the additional data it carries."));
    dl->addWidget(addContent_, 1);
    addContentImage_ = new ImageView;
    addContentImage_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    addContentImage_->setVisible(false);
    dl->addWidget(addContentImage_, 1);
    split->addWidget(dataBox);

    auto* structBox = new QWidget;
    auto* sl = new QVBoxLayout(structBox);
    sl->setContentsMargins(0, 0, 0, 0);
    auto* structTitle = new QLabel(QStringLiteral("File structure"));
    structTitle->setObjectName(QStringLiteral("SectionTitle"));
    sl->addWidget(structTitle);
    addStructure_ = new QTreeWidget;
    addStructure_->setColumnCount(2);
    addStructure_->setHeaderLabels({QStringLiteral("Field"), QStringLiteral("Value")});
    addStructure_->setRootIsDecorated(true);
    addStructure_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    addStructure_->header()->setStretchLastSection(true);
    sl->addWidget(addStructure_, 1);
    split->addWidget(structBox);
    split->setHandleWidth(10);
    split->setStyleSheet(QStringLiteral("QSplitter::handle { background: transparent; }"));
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    rl->addWidget(split, 1);
    root->addWidget(right, 70);

    connect(addRefresh_, &QPushButton::clicked, this, [this] { refreshAddFiles(); });
    connect(addBrowse_, &QPushButton::clicked, this, [this] {
        const QString start = addDirEdit_ ? addDirEdit_->text()
                                          : (decoder_ ? decoder_->outputRoot() : QString());
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Folder to search for additional data"), start);
        if (!chosen.isEmpty()) {
            addDirEdit_->setText(QDir::toNativeSeparators(chosen));
            refreshAddFiles();
        }
    });
    addDirEdit_->setText(QDir::toNativeSeparators(
        decoder_ != nullptr ? decoder_->outputRoot() : QStringLiteral("NadirStation_DATA")));
    connect(addFileTree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (current == nullptr) return;
                const QString path = current->data(0, Qt::UserRole).toString();
                if (!path.isEmpty()) showAddFile(path);
            });

    refreshAddFiles();
    return page;
}

void MainWindow::refreshAddFiles() {
    if (addFileTree_ == nullptr) return;
    addFileTree_->clear();
    const QString chosen = (addDirEdit_ != nullptr && !addDirEdit_->text().trimmed().isEmpty())
        ? addDirEdit_->text().trimmed()
        : (decoder_ != nullptr ? decoder_->outputRoot() : QStringLiteral("NadirStation_DATA"));
    QDir root(chosen);
    if (!root.exists()) {
        addSummary_->setText(QStringLiteral(
            "Output folder not found: %1\nReceive an observation, or import one, and rescan.")
            .arg(root.absolutePath()));
        return;
    }

    // One entry per transmission: the .lrit and the content written beside it are the same file.
    struct Entry { QString key, lrit, content; AddName name; qint64 dataBytes = 0; };
    QMap<QString, Entry> entries;
    QStringList filters{QStringLiteral("*.lrit"), QStringLiteral("*.LRIT")};
    for (const QString& s : addContentSuffixes()) filters << QStringLiteral("*") + s;
    QDirIterator it(root.absolutePath(), filters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        // Only what is inside a folder named ADD: the output root also holds FY-2 products and
        // GK-2A imagery, which share these suffixes.
        const QString rel = QDir::fromNativeSeparators(root.relativeFilePath(it.filePath()));
        const QStringList parts = rel.split(QLatin1Char('/'));
        const int addAt = parts.indexOf(QStringLiteral("ADD"));
        if (addAt < 0) continue;
        const AddFilePair pair = addFilePair(it.filePath());
        const QString key = !pair.lrit.isEmpty() ? pair.lrit : pair.content;
        Entry& e = entries[key];
        e.key = key;
        if (!pair.lrit.isEmpty()) e.lrit = pair.lrit;
        if (!pair.content.isEmpty()) e.content = pair.content;
    }

    // service -> data type -> entries
    QMap<int, QMap<QString, QList<Entry>>> grouped;
    for (Entry e : entries) {
        e.name = parseAddName(QFileInfo(e.key).fileName());
        e.dataBytes = QFileInfo(!e.content.isEmpty() ? e.content : e.lrit).size();
        const QString type = e.name.ok ? e.name.type : QString();
        grouped[static_cast<int>(addServiceOf(type))][type.isEmpty() ? QStringLiteral("(no ADD_ name)") : type].push_back(e);
    }

    int shown = 0;
    QTreeWidgetItem* first = nullptr;
    for (auto s = grouped.cbegin(); s != grouped.cend(); ++s) {
        const auto service = static_cast<AddService>(s.key());
        int count = 0;
        for (const auto& list : s.value()) count += list.size();
        auto* serviceItem = new QTreeWidgetItem(addFileTree_, {addServiceTitle(service), QStringLiteral("%1 file(s)").arg(count)});
        serviceItem->setToolTip(0, addServiceNote(service));
        QFont bold = serviceItem->font(0);
        bold.setBold(true);
        serviceItem->setFont(0, bold);
        serviceItem->setFlags(serviceItem->flags() & ~Qt::ItemIsSelectable);
        for (auto t = s.value().cbegin(); t != s.value().cend(); ++t) {
            auto* typeItem = new QTreeWidgetItem(serviceItem, {t.key(), QStringLiteral("%1").arg(t.value().size())});
            typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsSelectable);
            QList<Entry> list = t.value();
            // Newest dissemination first: a reader wants the current schedule or chart.
            std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
                if (a.name.disseminated != b.name.disseminated) return a.name.disseminated > b.name.disseminated;
                return a.key > b.key;
            });
            for (const Entry& e : list) {
                QString label;
                if (e.name.ok)
                    label = QStringLiteral("#%1  %2  seg %3").arg(e.name.sequence,
                        e.name.disseminated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), e.name.segment);
                else
                    label = QFileInfo(e.key).fileName();
                const QString fmt = e.content.isEmpty() ? QStringLiteral("raw only")
                                                        : contentFormat(e.content, {});
                auto* item = new QTreeWidgetItem(typeItem, {label, QStringLiteral("%1 · %2").arg(fmt, formatSize(e.dataBytes))});
                item->setData(0, Qt::UserRole, e.key);
                item->setData(0, Qt::UserRole + 1, e.content);
                item->setToolTip(0, QDir::toNativeSeparators(e.key));
                if (first == nullptr) first = item;
                ++shown;
            }
            typeItem->setExpanded(true);
        }
        serviceItem->setExpanded(true);
    }

    if (shown == 0) {
        addContent_->clear();
        addContent_->setVisible(true);
        addContentImage_->setVisible(false);
        addStructure_->clear();
        addSummary_->setText(QStringLiteral(
            "No additional data under %1.\nAdditional data is filed as <spacecraft>/ADD/<data type>/; "
            "an empty list usually means only imagery has been received.").arg(root.absolutePath()));
        return;
    }
    // The first file is opened rather than left for a click: an empty pane reads as a failed load.
    addFileTree_->setCurrentItem(first);
}

void MainWindow::showAddFile(const QString& path) {
    if (addStructure_ == nullptr || addContent_ == nullptr) return;
    addStructure_->clear();
    const AddFilePair pair = addFilePair(path);
    const QString key = !pair.lrit.isEmpty() ? pair.lrit : pair.content;
    const AddName name = parseAddName(QFileInfo(key).fileName());

    // The transmission, when it was kept.
    std::vector<std::uint8_t> raw;
    if (!pair.lrit.isEmpty()) {
        QFile f(pair.lrit);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray b = f.readAll();
            raw.assign(b.begin(), b.end());
        }
    }
    const bool headerValid = raw.size() >= 16 && raw[0] == 0 && raw[1] == 0 && raw[2] == 16;
    const nadirstation::gk2a::LritAddData add = headerValid ? nadirstation::gk2a::parseLritAddData(raw) : nadirstation::gk2a::LritAddData{};

    // The data field: the decoder's content file, else the bytes after the header records.
    nadirstation::gk2a::LritDataField field;
    if (!pair.content.isEmpty()) {
        QFile c(pair.content);
        if (c.open(QIODevice::ReadOnly)) {
            const QByteArray b = c.readAll();   // read once: two temporaries' iterators must not be mixed
            field.bytes.assign(b.begin(), b.end());
        }
        if (pair.content.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) {
            field.text = std::string(field.bytes.begin(), field.bytes.end());
            field.printable = true;
        }
    } else if (headerValid) {
        field = nadirstation::gk2a::lritDataField(raw);
    }
    const QString format = field.printable ? QStringLiteral("Text") : contentFormat(pair.content, field.bytes);
    field.looksLikeImage = format == QStringLiteral("GIF") || format == QStringLiteral("PNG") || format == QStringLiteral("JPEG");

    // ---- data field view
    QString shape;
    if (field.printable) {
        addContent_->setPlainText(QString::fromStdString(field.text));
        addContent_->setVisible(true);
        addContentImage_->setVisible(false);
        shape = QStringLiteral("%1 lines").arg(QString::fromStdString(field.text).count(QLatin1Char('\n')) + 1);
    } else if (field.looksLikeImage) {
        QImage chart;
        if (chart.loadFromData(field.bytes.data(), static_cast<int>(field.bytes.size()))) {
            addContentImage_->setImage(chart);
            addContentImage_->fitImage();
            addContentImage_->setVisible(true);
            addContent_->setVisible(false);
            shape = QStringLiteral("%1 × %2 px · wheel to zoom, drag to pan, double-click to fit").arg(chart.width()).arg(chart.height());
        } else {
            addContent_->setPlainText(QStringLiteral("The %1 data field could not be decoded (%2 bytes).").arg(format).arg(field.bytes.size()));
            addContent_->setVisible(true);
            addContentImage_->setVisible(false);
        }
    } else {
        QString hex;
        for (std::size_t i = 0; i < std::min<std::size_t>(256, field.bytes.size()); ++i) {
            hex += QStringLiteral("%1").arg(field.bytes[i], 2, 16, QLatin1Char('0'));
            hex += (i % 16 == 15) ? QStringLiteral("\n") : QStringLiteral(" ");
        }
        addContent_->setPlainText(field.bytes.empty()
            ? QStringLiteral("No data field.")
            : QStringLiteral("Binary data field, %1 bytes. First bytes:\n\n%2").arg(field.bytes.size()).arg(hex));
        addContent_->setVisible(true);
        addContentImage_->setVisible(false);
    }

    // ---- summary line
    const AddService service = addServiceOf(name.ok ? name.type : QString());
    QStringList head;
    head << addServiceTitle(service);
    if (name.ok) {
        head << name.type << QStringLiteral("#%1").arg(name.sequence)
             << name.disseminated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
    } else {
        head << QFileInfo(key).fileName();
    }
    head << QStringLiteral("%1, %2").arg(format, formatSize(static_cast<qint64>(field.bytes.size())));
    addSummary_->setText(head.join(QStringLiteral("  ·  ")) + (shape.isEmpty() ? QString() : QStringLiteral("\n") + shape));

    // ---- file structure: name fields, header records, data field
    auto* nameSec = structureSection(addStructure_, QStringLiteral("File name"));
    structureField(nameSec, QStringLiteral("Name"), QFileInfo(key).fileName());
    if (name.ok) {
        structureField(nameSec, QStringLiteral("File type"), QStringLiteral("ADD_ (additional data)"));
        structureField(nameSec, QStringLiteral("Data type"), name.type);
        structureField(nameSec, QStringLiteral("Service"), addServiceTitle(service));
        structureField(nameSec, QStringLiteral("Sequence No."), name.sequence);
        structureField(nameSec, QStringLiteral("Dissemination time"), name.disseminated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")));
        structureField(nameSec, QStringLiteral("Segment No."), name.segment);
    } else {
        structureField(nameSec, QStringLiteral("Form"), QStringLiteral("not ADD_<type>_<seq>_<date>_<time>_<seg>.lrit"));
    }

    if (pair.lrit.isEmpty()) {
        structureSection(addStructure_, QStringLiteral("Header records"), QStringLiteral("transmission (.lrit) not kept"));
    } else if (!headerValid) {
        structureSection(addStructure_, QStringLiteral("Header records"),
                         QStringLiteral("no valid primary header: damaged in reception or not an LRIT file"));
    } else {
        const std::uint64_t dataBits = add.dataFieldLength;
        const qint64 received = static_cast<qint64>(raw.size()) - static_cast<qint64>(add.totalHeaderLength);
        auto* prim = structureSection(addStructure_, QStringLiteral("Primary header (type 0)"));
        structureField(prim, QStringLiteral("File type code"),
                       QStringLiteral("%1 — %2").arg(add.fileTypeCode).arg(QString::fromStdString(add.kind)));
        structureField(prim, QStringLiteral("Total header length"), QStringLiteral("%1 B").arg(add.totalHeaderLength));
        structureField(prim, QStringLiteral("Data field length"),
                       QStringLiteral("%1 bits = %2 B").arg(dataBits).arg(dataBits / 8));
        structureField(prim, QStringLiteral("Received after header"),
                       QStringLiteral("%1 B%2").arg(received)
                           .arg(received == static_cast<qint64>(dataBits / 8) ? QStringLiteral(" (complete)")
                                                                              : QStringLiteral(" (differs from announced length)")));
        for (std::size_t r = 0; r < add.records.size(); ++r) {
            const auto& rec = add.records[r];
            if (rec.type == 0) continue;
            const std::size_t body = static_cast<std::size_t>(rec.offset) + 3;
            auto* sec = structureSection(addStructure_, QStringLiteral("%1 (type %2)").arg(recordTypeName(rec.type)).arg(rec.type));
            structureField(sec, QStringLiteral("Record length"), QStringLiteral("%1 B").arg(rec.recordLength));
            if (rec.type == 4) {
                structureField(sec, QStringLiteral("Text"), QString::fromStdString(rec.text).trimmed());
            } else if (rec.type == 5 && rec.recordLength >= 10) {
                // CCSDS day-segmented time: P-field, 2-byte day count from 1958-01-01, 4-byte ms of day.
                const int day = (raw[body + 1] << 8) | raw[body + 2];
                const qint64 ms = beU32(raw, body + 3);
                const QDateTime t = QDateTime(QDate(1958, 1, 1), QTime(0, 0), QTimeZone(QTimeZone::UTC)).addDays(day).addMSecs(ms);
                structureField(sec, QStringLiteral("Time (CCSDS CDS)"), t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz 'UTC'")));
            } else if (rec.type == 7 && rec.recordLength >= 7) {
                const std::uint32_t keyIndex = beU32(raw, body);
                structureField(sec, QStringLiteral("Key index"),
                               keyIndex == 0 ? QStringLiteral("0 — not encrypted") : QStringLiteral("%1 — encrypted").arg(keyIndex));
            } else if (rec.textual) {
                structureField(sec, QStringLiteral("Text"), QString::fromStdString(rec.text).trimmed());
            }
            for (const auto& kv : add.pairs)
                if (kv.recordIndex == static_cast<int>(r))
                    structureField(sec, QString::fromStdString(kv.key), QString::fromStdString(kv.value));
        }
    }

    auto* dataSec = structureSection(addStructure_, QStringLiteral("Data field"));
    if (headerValid) structureField(dataSec, QStringLiteral("Offset in file"), QStringLiteral("%1 B").arg(add.totalHeaderLength));
    structureField(dataSec, QStringLiteral("Format"), format);
    structureField(dataSec, QStringLiteral("Size"), QStringLiteral("%1 B").arg(field.bytes.size()));
    structureField(dataSec, QStringLiteral("Read from"),
                   pair.content.isEmpty() ? QStringLiteral("the transmission, after its header records")
                                          : QFileInfo(pair.content).fileName());
}

void MainWindow::setAddFolder(const QString& dir) {
    if (addDirEdit_ != nullptr) addDirEdit_->setText(QDir::toNativeSeparators(dir));
    refreshAddFiles();
}

void MainWindow::showAddFileByName(const QString& path) {
    showPageByName(QStringLiteral("add"));
    if (addFileTree_ == nullptr || path.isEmpty()) return;
    // Compared with separators normalised: the tree holds the path as the filesystem spelled it and
    // an argument arrives as the shell spelled it. Either the transmission or its content matches.
    const QString wanted = QDir::fromNativeSeparators(path);
    for (QTreeWidgetItemIterator it(addFileTree_); *it; ++it) {
        const QString a = QDir::fromNativeSeparators((*it)->data(0, Qt::UserRole).toString());
        const QString b = QDir::fromNativeSeparators((*it)->data(0, Qt::UserRole + 1).toString());
        if (!a.isEmpty() && (a == wanted || b == wanted)) {
            addFileTree_->setCurrentItem(*it);
            nadirstation::log::write(QStringLiteral("GUI --add-file: chose '%1'").arg(a));
            nadirstation::log::flush();
            return;
        }
    }
    // Not in the list: the folder field may be pointing elsewhere, so read it directly.
    nadirstation::log::write(QStringLiteral("GUI --add-file: '%1' is not in the list; reading it anyway").arg(path));
    nadirstation::log::flush();
    showAddFile(path);
}


