with open('src/components/TyresPage.cpp', 'r') as f:
    t_cpp = f.read()

start_sig = 'void MainWindow::updateTyreSetsTable() {'
end_sig = '}\n'

start_idx = t_cpp.find(start_sig)
if start_idx != -1:
    end_idx = t_cpp.find(end_sig, start_idx + len(start_sig))
    if end_idx != -1:
        new_body = '''void MainWindow::updateTyreSetsTable() {
    if (!tp_setsTable || lastTyreSetsData.empty() || !lastTyreSetsData.contains("sets")) return;

    std::vector<nlohmann::json> drySets;
    std::vector<nlohmann::json> wetSets;
    for (const auto& s : lastTyreSetsData["sets"]) {
        int compound = s.value("actual_compound", 0);
        if (compound != 0) {
            if (compound == 7 || compound == 8) {
                wetSets.push_back(s);
            } else {
                drySets.push_back(s);
            }
        }
    }

    auto sortSets = [](std::vector<nlohmann::json>& vec) {
        std::sort(vec.begin(), vec.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("idx", 99) < b.value("idx", 99);
        });
    };
    sortSets(drySets);
    sortSets(wetSets);

    int totalRows = 0;
    if (!drySets.empty()) totalRows += 1 + drySets.size();
    if (!wetSets.empty()) totalRows += 1 + wetSets.size();
    
    tp_setsTable->setRowCount(totalRows);
    
    // Clear any existing spans
    tp_setsTable->clearSpans();

    static const char* sessionLabels[] = {
        "—", "FP1", "FP2", "FP3", "Short P",
        "Q1", "Q2", "Q3", "Short Q", "1-Shot Q",
        "SS1", "SS2", "SS3", "SS Short", "SS 1-Shot",
        "Race", "Race 2", "Race 3", "Time Trial"
    };

    int row = 0;
    auto makeItem = [](const QString& text) { return new QTableWidgetItem(text); };

    auto addSectionHeader = [&](const QString& text) {
        auto* item = makeItem(text);
        item->setFlags(Qt::NoItemFlags);
        QFont f = item->font();
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        f.setPointSize(8);
        item->setFont(f);
        item->setForeground(QColor("#8A8D9F"));
        
        tp_setsTable->setItem(row, 0, item);
        for(int c=1; c<7; ++c) {
            auto* emptyItem = makeItem("");
            emptyItem->setFlags(Qt::NoItemFlags);
            tp_setsTable->setItem(row, c, emptyItem);
        }
        tp_setsTable->setSpan(row, 0, 1, 7);
        tp_setsTable->setRowHeight(row, 32);
        row++;
    };

    auto addSetRow = [&](const nlohmann::json& s) {
        int idx        = s.value("idx", 0);
        int compound   = s.value("actual_compound",     0);
        int visual     = s.value("visual_compound",     0);
        int wear       = s.value("wear",                0);
        int lifeSpan   = s.value("life_span",           0);
        int usable     = s.value("usable_life",         0);
        int recSess    = s.value("recommended_session", 0);
        int deltaMs    = s.value("lap_delta_ms",        0);

        QString status    = setStatusText(s);
        QColor  statusCol = setStatusColor(s);
        QColor  cmpFg     = tyreTextColor(visual);

        // Col 0: #
        tp_setsTable->setItem(row, 0, makeItem(QString::number(idx + 1)));

        // Col 1: Compound
        auto* cmpItem = makeItem(tyreLabel(compound));
        if (cmpFg.isValid()) cmpItem->setForeground(cmpFg);
        tp_setsTable->setItem(row, 1, cmpItem);

        // Col 2: Status
        auto* stItem = makeItem(status);
        stItem->setForeground(statusCol);
        tp_setsTable->setItem(row, 2, stItem);

        // Col 3: Wear — bar + percentage label
        {
            const QString wc = wearPctColor(wear).name();
            QWidget* cell = new QWidget;
            cell->setStyleSheet("background: transparent;");
            QHBoxLayout* wh = new QHBoxLayout(cell);
            wh->setContentsMargins(4, 0, 4, 0);
            wh->setSpacing(4);

            auto* bar = new QProgressBar;
            bar->setRange(0, 100);
            bar->setValue(wear);
            bar->setTextVisible(false);
            bar->setFixedHeight(6);
            bar->setStyleSheet(QString(
                "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                "QProgressBar::chunk { background: %1; border-radius: 3px; }"
            ).arg(wc));

            auto* wearLbl = new QLabel(QString::number(wear) + "%");
            wearLbl->setStyleSheet("color: " + wc + "; font-weight: bold; background: transparent;");
            QFont wf; wf.setPointSize(8);
            wearLbl->setFont(wf);
            wearLbl->setFixedWidth(36);
            wearLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            wh->addWidget(bar, 1);
            wh->addWidget(wearLbl);
            tp_setsTable->setCellWidget(row, 3, cell);
        }

        // Col 4: Life
        QString lifeText = (lifeSpan > 0 || usable > 0)
            ? QString("%1/%2L").arg(lifeSpan).arg(usable) : "—";
        tp_setsTable->setItem(row, 4, makeItem(lifeText));

        // Col 5: Recommended session
        int rsIdx = (recSess >= 0 && recSess < 19) ? recSess : 0;
        tp_setsTable->setItem(row, 5, makeItem(sessionLabels[rsIdx]));

        // Col 6: Lap delta (seconds, no unit suffix)
        QString deltaText;
        if (deltaMs != 0)
            deltaText = QString("%1%2").arg(deltaMs > 0 ? "+" : "").arg(deltaMs / 1000.0, 0, 'f', 3);
        auto* deltaItem = makeItem(deltaText);
        if (deltaMs > 0)      deltaItem->setForeground(QColor("#C4162A"));
        else if (deltaMs < 0) deltaItem->setForeground(QColor("#37872D"));
        tp_setsTable->setItem(row, 6, deltaItem);

        tp_setsTable->setRowHeight(row, 22);
        row++;
    };

    if (!drySets.empty()) {
        addSectionHeader("DRY SETS (SLICKS)");
        for (const auto& s : drySets) addSetRow(s);
    }
    
    if (!wetSets.empty()) {
        addSectionHeader("WET / INTER SETS");
        for (const auto& s : wetSets) addSetRow(s);
    }'''
        t_cpp = t_cpp[:start_idx] + new_body + t_cpp[end_idx:]

with open('src/components/TyresPage.cpp', 'w') as f:
    f.write(t_cpp)

print("Done")
