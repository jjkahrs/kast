#pragma once

#include "event_bus.h"
#include "gui_access.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Game GUI lists (docs/engine/game-gui.html, "Lists"): vertical, single-select, scrolled by the wheel and the scrollbar,
// styled by kGuiWidgetStyle unless the document's own sheets say otherwise.
// - List Box: <div class="listbox">. Its rows are its children with class "row", authored or made by a data-for row
//   template. A row's value is its data-value, else its 0-based index (a number). The selected row has class "selected"
//   (authored on a row, it is selected from the start).
// - Virtual List: <div class="virtuallist" data-row-height="24" data-key="id"> holding exactly one row template, e.g.
//   <div class="row" data-for="item : items">{{item.name}}</div>, inside a data model (data-model on the list or an
//   ancestor); items is a top-level array of that model (kast.gui.set). Only the rows in view and kGuiListBuffer more
//   above and below are elements, recycled as it scrolls, between two spacers that keep the scroll range at rows * row
//   height. Rows are data-row-height pixels high (missing or not a positive number: kGuiListRowHeight), set by the
//   engine on every row. A row's value is its item's data-key field as a string, else its index. When the array is
//   replaced the selection follows its key (its index without one); a selected item that is gone clears it. The index
//   alias of the data-for (it_index) counts from the first live row, not the first item.
// Both: a click selects a row and focuses the list; while it has focus Up / Down / Home / End / PageUp / PageDown move the
// selection and scroll it into view. data-event="name" publishes "name" with {"id": <the list's id or "">, "document":
// <the .rml path>, "value": <the new value, nil if none>} from publisher "UI" when the player changes the selection;
// kast.gui.setValue and data binding publish nothing. data-follow: rows added while the view is at the end keep it there;
// once the player scrolls up it stays put.
namespace kast {

constexpr int kGuiListBuffer = 4;
constexpr float kGuiListRowHeight = 24.0f;

struct GuiRowRange {
    int first = 0;
    int count = 0;
    bool operator==(const GuiRowRange&) const = default;
};
// The rows of a virtual list to keep as elements: those [scrollTop, scrollTop + viewHeight) shows plus buffer more on
// either side, within 0 .. rows - 1.
GuiRowRange guiVisibleRows(float scrollTop, float viewHeight, float rowHeight, int rows, int buffer = kGuiListBuffer);
// The scroll offset that shows [rowTop, rowTop + rowHeight) moving the view least: scrollTop if it is in view already.
float guiScrollIntoView(float scrollTop, float viewHeight, float rowTop, float rowHeight);

enum class GuiListKey { Up, Down, PageUp, PageDown, Home, End };
// The row key moves the selection to from selected (-1: none; then Up and End go to the last row, the others to the
// first) among rows, page rows a page; -1 if there are no rows.
int guiListMove(GuiListKey key, int selected, int rows, int page);

// The field of item (an object) as a string: a string as is, a number as Lua prints it ("42", "2.5"); nullopt otherwise.
std::optional<std::string> guiRowKey(const GuiValue& item, std::string_view field);
// Where the selection of row index (with key, nullopt if it had none) is once rows was replaced: with a key, the first
// row of that key, else -1; without one the same index if rows still has it, else -1.
int guiTrackRow(const GuiValue& rows, std::string_view field, int index, const std::optional<std::string>& key);

} // namespace kast

namespace Rml {
class Element;
class ElementDocument;
} // namespace Rml

namespace kast {

class GuiModels;
struct GuiWindow;

// The lists of one document; bus and models must outlive it. Hears its Load (in capture, on the context's root, while
// the document loads) and Click, Keydown on the document.
class ListListener final : public Rml::EventListener {
public:
    ListListener(EventBus& bus, std::string document, GuiModels& models)
        : bus_(bus), document_(std::move(document)), models_(models) {}
    // On the document's Load event, before RmlUi first updates its data models: holds each virtual list's source empty
    // (GuiModels::hold), so its row template makes no rows. The loader calls GuiModels::restore once attach() ran.
    void hold(Rml::ElementDocument& document);
    // Once the document has loaded: finds its lists and turns each virtual list's row template into one over a window
    // of its source. A virtual list that isn't one (no data-for row, no data model) logs a warning and stays as
    // written: a data-for over every item.
    void attach(Rml::ElementDocument& document);
    // Once a frame before the documents update: windows follow the scroll position and their source, data-follow and
    // scrollTo take effect once the rows they need are laid out, the selected virtual row is marked.
    void update();
    void ProcessEvent(Rml::Event& event) override;

    // The list's value (see above; null if no row is selected); nullopt if element is no list.
    std::optional<GuiValue> value(Rml::Element& element);
    // Selects as kast.gui.setValue: a string the row of that value (key), a number the row of that index, null none.
    // False (and why in error, never naming the value) if element is no list or no row matches.
    bool select(Rml::Element& element, const GuiValue& value, std::string& error);
    // Scrolls row index (kEnd: the end) of the list into view, now or once laid out. False (error) if element is no list
    // or has no such row.
    static constexpr int kEnd = std::numeric_limits<int>::min();
    bool scrollTo(Rml::Element& element, int index, std::string& error);

private:
    // An element that may be gone. ponytail: optional only because RmlUi's DLL doesn't export ObserverPtr's default
    // constructor.
    class Ref {
    public:
        Ref& operator=(Rml::ObserverPtr<Rml::Element> element)
        {
            ptr_ = std::move(element);
            return *this;
        }
        Rml::Element* get() const { return ptr_ ? ptr_->get() : nullptr; }

    private:
        std::optional<Rml::ObserverPtr<Rml::Element>> ptr_;
    };

    struct List {
        Ref element;
        GuiWindow* window = nullptr; // a virtual list's; null for a list box
        std::string model;           // virtual: its data model, key field, spacers, row height, selection
        std::string keyField;
        Ref top;
        Ref bottom;
        float rowHeight = kGuiListRowHeight;
        int selected = -1;
        std::optional<std::string> key;
        int version = 0;
        float topHeight = -1.0f; // the spacer heights last set
        float bottomHeight = -1.0f;
        int rows = 0;       // as of the last update; -1 before the first
        bool atEnd = true;  // the view was at the end when last laid out
        std::optional<int> pending; // a row (or kEnd) to scroll to once laid out
        int pendingAge = 0;         // updates since it was asked for
    };

    List* find(Rml::Element& element);
    List* listOf(Rml::Element* element); // the list element is or is inside (a list box is taken on first use)
    struct Template { // a virtual list's row template, its data-for parts and data model
        Rml::Element* element = nullptr;
        std::string aliases; // "item" of "item : items"; empty if it names none
        std::string source;
        std::string model;
    };
    static std::optional<Template> rowTemplate(Rml::Element& list); // nullopt if it isn't a virtual list
    void makeVirtual(Rml::Element& element);
    std::vector<Rml::Element*> rowElements(Rml::Element& element) const;
    int rowCount(List& list);
    int selectedRow(List& list);
    void setSelected(List& list, int row, bool publish);
    bool laidOut(List& list);
    void scrollIntoView(List& list, int row);
    void markRows(List& list);

    EventBus& bus_;
    std::string document_;
    GuiModels& models_;
    std::deque<List> lists_; // a deque: a list box added on first use leaves the others where they are
};

} // namespace kast
