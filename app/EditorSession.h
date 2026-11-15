#pragma once

#include "DocumentModule.h"
#include "DocxExportTypes.h"
#include "HitTest.h"
#include "LayoutResult.h"

#include <QHash>
#include <QObject>

namespace editor::app {

class EditorSession : public QObject {
    Q_OBJECT

public:
    explicit EditorSession(QObject* parent = nullptr);

    bool newDocument();
    bool loadSnapshot(const data::DocumentSnapshot& snapshot);
    std::optional<data::ContainerId> insertImage(const QString& path, data::PlacementMode placement);

    const data::DocumentSnapshot& snapshot() const { return snapshot_; }
    const data::LayoutResult& layout() const { return layout_; }
    const interaction::InteractionState& state() const { return state_; }
    data::TextStyle currentStyle() const;
    const data::ParagraphInfo* currentParagraph() const;

    void select(const interaction::Hit& anchor, const interaction::Hit& focus);
    bool key(const interaction::KeyInput& key, const QString& clipboardText);
    bool ime(const interaction::ImeInput& ime);
    bool action(const interaction::EditAction& action);
    io::DocxExportResult exportTo(const QString& path) const;

signals:
    void changed(bool layoutChanged);
    void clipboardRequested(const QString& text);
    void editRejected(const QString& reason);

private:
    void startDocument(document::Document document, const data::ContainerId& textId);
    void setLayoutSnapshot(data::DocumentSnapshot snapshot);
    bool accept(interaction::InteractionOutput output);

    document::Document document_;
    data::DocumentSnapshot snapshot_;
    data::LayoutResult layout_;
    interaction::InteractionState state_;
    QHash<QString, data::ResourceId> importedImages_;
};

} // namespace editor::app
