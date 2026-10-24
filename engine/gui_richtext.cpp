#include "gui_richtext.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>

namespace kast {
namespace {

constexpr double kMinSize = 4.0;
constexpr double kMaxSize = 200.0;
constexpr int kMaxImageSide = 4096;

// text escaped for RML text or a quoted attribute value; for rich text also braces (no {{...}} data binding).
void appendEscaped(std::string& out, std::string_view text, bool rich)
{
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\n': out += "<br/>"; break;
        case '{': out += rich ? "&#123;" : "{"; break;
        case '}': out += rich ? "&#125;" : "}"; break;
        default: out += c; break;
        }
    }
}

std::string escaped(std::string_view text)
{
    std::string out;
    appendEscaped(out, text, true);
    return out;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

bool printable(char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; }

std::optional<std::string> colorValue(std::string_view value)
{
    static constexpr std::string_view kNames[] = {"black",  "silver", "gray",  "grey",   "white", "maroon", "red",
                                                  "orange", "purple", "fuchsia", "green", "lime",  "olive",  "yellow",
                                                  "navy",   "blue",   "teal",  "aqua",   "transparent"};
    if (value.starts_with('#')) {
        const std::string_view hex = value.substr(1);
        const bool digits = std::ranges::all_of(hex, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        if (digits && (hex.size() == 3 || hex.size() == 4 || hex.size() == 6 || hex.size() == 8))
            return std::string(value);
        return std::nullopt;
    }
    const std::string name = lower(value);
    return std::ranges::find(kNames, name) != std::end(kNames) ? std::optional(name) : std::nullopt;
}

std::optional<double> number(std::string_view text)
{
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// One opening tag read: its markup, or why it can't be one. note: a problem that still let it through.
struct Tag {
    std::string name;
    std::string markup;
    bool closes = true; // false: [img], nothing to close
    std::string error;
    std::string note;
};

Tag readTag(std::string_view body, const GuiFontKnown& fontKnown)
{
    Tag tag;
    const size_t equals = body.find('=');
    tag.name = lower(body.substr(0, equals));
    const bool hasValue = equals != std::string_view::npos;
    const std::string_view value = hasValue ? body.substr(equals + 1) : std::string_view();
    const auto fail = [&](std::string why) {
        tag.error = std::move(why);
        return tag;
    };
    const auto span = [&](std::string_view style) {
        tag.markup = std::format(R"(<span style="{}">)", style);
        return tag;
    };
    if (tag.name == "b" || tag.name == "i" || tag.name == "u" || tag.name == "s") {
        if (hasValue)
            return fail(std::format("[{}] takes no value", tag.name));
        return span(tag.name == "b"   ? "font-weight: bold;"
                    : tag.name == "i" ? "font-style: italic;"
                    : tag.name == "u" ? "text-decoration: underline;"
                                      : "text-decoration: line-through;");
    }
    if (tag.name == "color") {
        const std::optional<std::string> color = colorValue(value);
        if (!color)
            return fail("not a colour (#rgb, #rgba, #rrggbb, #rrggbbaa or a colour name)");
        return span(std::format("color: {};", *color));
    }
    if (tag.name == "size") {
        const std::optional<double> size = number(value);
        if (!size || *size <= 0.0)
            return fail("not a positive number of pixels");
        const double clamped = std::clamp(*size, kMinSize, kMaxSize);
        if (clamped != *size)
            tag.note = std::format("[size={}] is clamped to {}", value, clamped);
        return span(std::format("font-size: {}px;", clamped));
    }
    if (tag.name == "font") {
        const auto nameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '_' || c == '-'; };
        const size_t first = value.find_first_not_of(' ');
        if (first == std::string_view::npos || !std::ranges::all_of(value, nameChar))
            return fail("a font family is letters, digits, spaces, _ and - only");
        const std::string_view family = value.substr(first, value.find_last_not_of(' ') - first + 1);
        if (fontKnown && !fontKnown(family))
            return fail(std::format("no font family '{}' is loaded", family));
        return span(std::format("font-family: {};", family));
    }
    if (tag.name == "link") {
        if (value.empty() || !std::ranges::all_of(value, printable))
            return fail("a link target is printable text, not empty");
        tag.markup = std::format(R"(<span class="link" data-link="{}">)", escaped(value));
        return tag;
    }
    if (tag.name == "img") {
        tag.closes = false;
        std::vector<std::string_view> parts;
        for (size_t start = 0; start <= value.size();) {
            const size_t space = std::min(value.find(' ', start), value.size());
            if (space > start)
                parts.push_back(value.substr(start, space - start));
            start = space + 1;
        }
        const auto pathChar = [](char c) { return printable(c) && c != '"' && c != '\'' && c != '<' && c != '>'; };
        if (parts.empty() || !std::ranges::all_of(parts[0], pathChar))
            return fail("an image path is printable text without quotes or < >");
        std::optional<int> side[2]; // w, h
        for (size_t i = 1; i < parts.size(); ++i) {
            const std::string key = lower(parts[i].substr(0, 2));
            const std::optional<double> n = number(parts[i].substr(std::min<size_t>(2, parts[i].size())));
            const int which = key == "w=" ? 0 : key == "h=" ? 1 : -1;
            if (which < 0 || side[which] || !n || *n != std::floor(*n) || *n < 1.0 || *n > kMaxImageSide)
                return fail(std::format("image sizes are w=N and h=N, whole pixels 1..{}", kMaxImageSide));
            side[which] = static_cast<int>(*n);
        }
        std::string style = side[0] || side[1] ? "" : "height: 1em;";
        if (side[0])
            style += std::format("width: {}px;", *side[0]);
        if (side[1])
            style += std::format("height: {}px;", *side[1]);
        tag.markup = std::format(R"(<img src="{}" style="{}"/>)", escaped(parts[0]), style);
        return tag;
    }
    return fail("unknown tag");
}

} // namespace

std::string guiTextRml(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    appendEscaped(out, text, false);
    return out;
}

std::string guiRichTextRml(std::string_view bbcode, std::vector<GuiRichTextProblem>* problems, const GuiFontKnown& fontKnown)
{
    struct Open {
        std::string name;
        std::string markup;
        size_t offset = 0;
    };
    constexpr std::string_view kClose = "</span>";
    std::string out;
    out.reserve(bbcode.size() * 2);
    std::vector<Open> open; // innermost last
    const auto problem = [&](size_t offset, std::string message) {
        if (problems)
            problems->push_back({offset, std::move(message)});
    };
    // RmlUi draws no text node that is only spaces ("[b]a[/b] [i]b[/i]" would read "ab"): such a run waits for what
    // follows it and goes after the text the spans just closed end with, else before following text or into the span
    // opened next, else (an image or a closing tag follows) as &nbsp;. Spaces at the end are dropped, as layout would.
    bool space = false;
    const auto spaceInClosed = [&] { // after the text the spans just closed end with, if they end with text
        size_t at = out.size();
        while (std::string_view(out).substr(0, at).ends_with(kClose))
            at -= kClose.size();
        if (!space || at == out.size() || at == 0 || out[at - 1] == '>')
            return;
        out.insert(at, " ");
        space = false;
    };
    enum class Next { Text, Open, Other };
    const auto write = [&](Next next, std::string_view markup) {
        spaceInClosed();
        if (space && next == Next::Text)
            out += ' ';
        else if (space && next == Next::Other)
            out += "&nbsp;";
        if (next == Next::Text)
            appendEscaped(out, markup, true);
        else
            out += markup;
        if (space && next == Next::Open)
            out += ' ';
        space = false;
    };
    const auto text = [&](std::string_view run) {
        if (!run.empty() && run.find_first_not_of(" \t") == std::string_view::npos)
            space = true;
        else if (!run.empty())
            write(Next::Text, run);
    };
    size_t pending = 0; // start of the text not yet written
    for (size_t at = 0; at < bbcode.size();) {
        if (bbcode[at] != '[') {
            ++at;
            continue;
        }
        text(bbcode.substr(pending, at - pending));
        const size_t close = bbcode.find(']', at + 1);
        if (at + 1 < bbcode.size() && bbcode[at + 1] == '[') { // "[[": a literal "["
            write(Next::Text, "[");
            pending = at += 2;
            continue;
        }
        if (close == std::string_view::npos || bbcode.find('[', at + 1) < close) {
            problem(at, "'[' without ']' shows as text (write [[ for a literal [)");
            write(Next::Text, "[");
            pending = at += 1;
            continue;
        }
        const std::string_view whole = bbcode.substr(at, close - at + 1);
        const std::string_view body = whole.substr(1, whole.size() - 2);
        if (body.starts_with('/')) {
            const std::string name = lower(body.substr(1));
            const auto found = std::ranges::find(open.rbegin(), open.rend(), name, &Open::name);
            if (found == open.rend()) {
                problem(at, std::format("{} closes nothing that is open: shows as text", whole));
                write(Next::Text, whole);
            } else {
                const size_t index = static_cast<size_t>(open.rend() - found) - 1;
                std::vector<Open> inner(open.begin() + static_cast<std::ptrdiff_t>(index) + 1, open.end());
                for (const Open& skipped : inner)
                    problem(at, std::format("{} also closes [{}] inside it (mis-nested): [{}] opens again after it", whole,
                                            skipped.name, skipped.name));
                std::string markup;
                for (size_t n = open.size(); n > index; --n)
                    markup += kClose;
                open.resize(index);
                for (Open& again : inner) {
                    markup += again.markup;
                    open.push_back(std::move(again));
                }
                write(Next::Other, markup);
            }
        } else {
            Tag tag = readTag(body, fontKnown);
            if (!tag.error.empty()) {
                problem(at, std::format("{}: {}: shows as text", whole, tag.error));
                write(Next::Text, whole);
            } else {
                if (!tag.note.empty())
                    problem(at, tag.note);
                write(tag.closes ? Next::Open : Next::Other, tag.markup);
                if (tag.closes)
                    open.push_back({tag.name, tag.markup, at});
            }
        }
        pending = at = close + 1;
    }
    text(bbcode.substr(pending));
    spaceInClosed();
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
        problem(it->offset, std::format("[{}] is not closed: closed at the end", it->name));
        out += kClose;
    }
    return out;
}

} // namespace kast
