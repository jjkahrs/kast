#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Game GUI rich text (docs/engine/game-gui.html, "Rich text"): BBCode turned into safe RML, no RmlUi needed. Tag names
// are case-insensitive; values are taken as written (no spaces around '=').
// - [b] [i] [u] [s]: bold, italic, underline, strikethrough (RmlUi draws one text decoration: an inner [u] / [s] wins).
// - [color=X]: X is #rgb, #rgba, #rrggbb, #rrggbbaa or a colour name RmlUi knows (black, silver, gray, grey, white,
//   maroon, red, orange, purple, fuchsia, green, lime, olive, yellow, navy, blue, teal, aqua, transparent).
// - [size=N]: font size in pixels, a positive number, clamped to 4..200.
// - [font=Family]: a loaded font family ([A-Za-z0-9 _-] only; the bundled Roboto, or a project font from fonts/).
// - [link=target]...[/link]: a clickable <span class="link" data-link="target"> (gui_events.h publishes gui.link); target
//   is any printable text.
// - [img=path] or [img=path w=N h=N]: an <img> with no closing tag; path relative to the .rml, w / h pixels (1..4096).
//   Without w and h it is one line high (1em); with one of them the other follows the image's aspect.
// Everything else is text, escaped as guiTextRml does (and { } as &#123; &#125;, so no {{...}} binding runs); "\n" is
// <br/>. "[[" is a literal "[" ("]" needs no escape). Spaces alone between two tags are kept (RmlUi draws no text node
// that is only spaces): moved to the end of the text the spans just closed hold, or into the text after them, else
// written as &nbsp;.
// Malformed markup never fails: an unknown tag, a bad value, a "[" with no "]" and a closing tag with nothing open to
// close show as the literal text they are; tags still open at the end are closed there. A closing tag that skips inner
// open tags ([b]a[i]b[/b]c[/i]) closes them with it and opens them again after it: <b>a<i>b</i></b><i>c</i>. Each case
// is reported as a GuiRichTextProblem.
namespace kast {

// text as RML: &, <, > and " escaped, "\n" as <br/> (GuiAccess::setText on an element that isn't a text field).
std::string guiTextRml(std::string_view text);

struct GuiRichTextProblem {
    size_t offset = 0; // byte offset of the tag (or "[") in the BBCode
    std::string message;
};

// Whether a font family is loaded (gui::fontFamilyLoaded); a [font=] naming one that isn't is a bad value. Empty: any
// well-formed name is taken.
using GuiFontKnown = std::function<bool(std::string_view family)>;

// bbcode as RML (see above); its problems are appended to problems if given.
std::string guiRichTextRml(std::string_view bbcode, std::vector<GuiRichTextProblem>* problems = nullptr,
                           const GuiFontKnown& fontKnown = {});

} // namespace kast
