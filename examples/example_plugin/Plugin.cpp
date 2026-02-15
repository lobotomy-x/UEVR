/*
This file (Plugin.cpp) is licensed under the MIT license and is separate from the rest of the UEVR codebase.

Copyright (c) 2023 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <sstream>
#include <mutex>
#include <memory>
#include <locale>
#include <codecvt>

#include <Windows.h>
#include <filesystem>
// only really necessary if you want to render to the screen
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"
 #include "rendering/shared.hpp"

#include "uevr/Plugin.hpp"
        #include <algorithm>
#include <chrono>
#include <string>
#include <regex>
#include <cmath>
#include <limits>
#include <fstream>



#define IMGUI_DEFINE_MATH_OPERATORS



template <class InputIt1, class InputIt2, class BinaryPredicate>
bool equals(InputIt1 first1, InputIt1 last1, InputIt2 first2, InputIt2 last2, BinaryPredicate p) {
    for (; first1 != last1 && first2 != last2; ++first1, ++first2) {
        if (!p(*first1, *first2))
            return false;
    }
    return first1 == last1 && first2 == last2;
}

TextEditor::TextEditor()
    : mLineSpacing(1.0f)
    , mUndoIndex(0)
    , mTabSize(4)
    , mOverwrite(false)
    , mReadOnly(false)
    , mWithinRender(false)
    , mScrollToCursor(false)
    , mScrollToTop(false)
    , mTextChanged(false)
    , mColorizerEnabled(true)
    , mTextStart(20.0f)
    , mLeftMargin(10)
    , mCursorPositionChanged(false)
    , mColorRangeMin(0)
    , mColorRangeMax(0)
    , mSelectionMode(SelectionMode::Normal)
    , mCheckComments(true)
    , mLastClick(-1.0f)
    , mHandleKeyboardInputs(true)
    , mHandleMouseInputs(true)
    , mIgnoreImGuiChild(false)
    , mShowWhitespaces(true)
    , mStartTime(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) {
    SetPalette(GetDarkPalette());
    SetLanguageDefinition(LanguageDefinition::Lua());
    mLines.push_back(Line());
}

TextEditor::~TextEditor() {
}

void TextEditor::SetLanguageDefinition(const LanguageDefinition& aLanguageDef) {
    mLanguageDefinition = aLanguageDef;
    mRegexList.clear();

    for (auto& r : mLanguageDefinition.mTokenRegexStrings)
        mRegexList.push_back(std::make_pair(std::regex(r.first, std::regex_constants::optimize), r.second));

    Colorize();
}

void TextEditor::SetPalette(const Palette& aValue) {
    mPaletteBase = aValue;
}

std::string TextEditor::GetText(const Coordinates& aStart, const Coordinates& aEnd) const {
    std::string result;

    auto lstart = aStart.mLine;
    auto lend = aEnd.mLine;
    auto istart = GetCharacterIndex(aStart);
    auto iend = GetCharacterIndex(aEnd);
    size_t s = 0;

    for (size_t i = lstart; i < lend; i++)
        s += mLines[i].size();

    result.reserve(s + s / 8);

    while (istart < iend || lstart < lend) {
        if (lstart >= (int)mLines.size())
            break;

        auto& line = mLines[lstart];
        if (istart < (int)line.size()) {
            result += line[istart].mChar;
            istart++;
        } else {
            istart = 0;
            ++lstart;
            result += '\n';
        }
    }

    return result;
}

TextEditor::Coordinates TextEditor::GetActualCursorCoordinates() const {
    return SanitizeCoordinates(mState.mCursorPosition);
}

TextEditor::Coordinates TextEditor::SanitizeCoordinates(const Coordinates& aValue) const {
    auto line = aValue.mLine;
    auto column = aValue.mColumn;
    if (line >= (int)mLines.size()) {
        if (mLines.empty()) {
            line = 0;
            column = 0;
        } else {
            line = (int)mLines.size() - 1;
            column = GetLineMaxColumn(line);
        }
        return Coordinates(line, column);
    } else {
        column = mLines.empty() ? 0 : min(column, GetLineMaxColumn(line));
        return Coordinates(line, column);
    }
}

// https://en.wikipedia.org/wiki/UTF-8
// We assume that the char is a standalone character (<128) or a leading byte of an UTF-8 code sequence (non-10xxxxxx code)
static int UTF8CharLength(TextEditor::Char c) {
    if ((c & 0xFE) == 0xFC)
        return 6;
    if ((c & 0xFC) == 0xF8)
        return 5;
    if ((c & 0xF8) == 0xF0)
        return 4;
    else if ((c & 0xF0) == 0xE0)
        return 3;
    else if ((c & 0xE0) == 0xC0)
        return 2;
    return 1;
}

// "Borrowed" from ImGui source
static inline int ImTextCharToUtf8(char* buf, int buf_size, unsigned int c) {
    if (c < 0x80) {
        buf[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        if (buf_size < 2)
            return 0;
        buf[0] = (char)(0xc0 + (c >> 6));
        buf[1] = (char)(0x80 + (c & 0x3f));
        return 2;
    }
    if (c >= 0xdc00 && c < 0xe000) {
        return 0;
    }
    if (c >= 0xd800 && c < 0xdc00) {
        if (buf_size < 4)
            return 0;
        buf[0] = (char)(0xf0 + (c >> 18));
        buf[1] = (char)(0x80 + ((c >> 12) & 0x3f));
        buf[2] = (char)(0x80 + ((c >> 6) & 0x3f));
        buf[3] = (char)(0x80 + ((c) & 0x3f));
        return 4;
    }
    // else if (c < 0x10000)
    {
        if (buf_size < 3)
            return 0;
        buf[0] = (char)(0xe0 + (c >> 12));
        buf[1] = (char)(0x80 + ((c >> 6) & 0x3f));
        buf[2] = (char)(0x80 + ((c) & 0x3f));
        return 3;
    }
}

void TextEditor::Advance(Coordinates& aCoordinates) const {
    if (aCoordinates.mLine < (int)mLines.size()) {
        auto& line = mLines[aCoordinates.mLine];
        auto cindex = GetCharacterIndex(aCoordinates);

        if (cindex + 1 < (int)line.size()) {
            auto delta = UTF8CharLength(line[cindex].mChar);
            cindex = min(cindex + delta, (int)line.size() - 1);
        } else {
            ++aCoordinates.mLine;
            cindex = 0;
        }
        aCoordinates.mColumn = GetCharacterColumn(aCoordinates.mLine, cindex);
    }
}

void TextEditor::DeleteRange(const Coordinates& aStart, const Coordinates& aEnd) {
    assert(aEnd >= aStart);
    assert(!mReadOnly);

    // printf("D(%d.%d)-(%d.%d)\n", aStart.mLine, aStart.mColumn, aEnd.mLine, aEnd.mColumn);

    if (aEnd == aStart)
        return;

    auto start = GetCharacterIndex(aStart);
    auto end = GetCharacterIndex(aEnd);

    if (aStart.mLine == aEnd.mLine) {
        auto& line = mLines[aStart.mLine];
        auto n = GetLineMaxColumn(aStart.mLine);
        if (aEnd.mColumn >= n)
            line.erase(line.begin() + start, line.end());
        else
            line.erase(line.begin() + start, line.begin() + end);
    } else {
        auto& firstLine = mLines[aStart.mLine];
        auto& lastLine = mLines[aEnd.mLine];

        firstLine.erase(firstLine.begin() + start, firstLine.end());
        lastLine.erase(lastLine.begin(), lastLine.begin() + end);

        if (aStart.mLine < aEnd.mLine)
            firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());

        if (aStart.mLine < aEnd.mLine)
            RemoveLine(aStart.mLine + 1, aEnd.mLine + 1);
    }

    mTextChanged = true;
}

int TextEditor::InsertTextAt(Coordinates& /* inout */ aWhere, const char* aValue) {
    assert(!mReadOnly);

    int cindex = GetCharacterIndex(aWhere);
    int totalLines = 0;
    while (*aValue != '\0') {
        assert(!mLines.empty());

        if (*aValue == '\r') {
            // skip
            ++aValue;
        } else if (*aValue == '\n') {
            if (cindex < (int)mLines[aWhere.mLine].size()) {
                auto& newLine = InsertLine(aWhere.mLine + 1);
                auto& line = mLines[aWhere.mLine];
                newLine.insert(newLine.begin(), line.begin() + cindex, line.end());
                line.erase(line.begin() + cindex, line.end());
            } else {
                InsertLine(aWhere.mLine + 1);
            }
            ++aWhere.mLine;
            aWhere.mColumn = 0;
            cindex = 0;
            ++totalLines;
            ++aValue;
        } else {
            auto& line = mLines[aWhere.mLine];
            auto d = UTF8CharLength(*aValue);
            while (d-- > 0 && *aValue != '\0')
                line.insert(line.begin() + cindex++, Glyph(*aValue++, PaletteIndex::Default));
            ++aWhere.mColumn;
        }

        mTextChanged = true;
    }

    return totalLines;
}

void TextEditor::AddUndo(UndoRecord& aValue) {
    assert(!mReadOnly);
    // printf("AddUndo: (@%d.%d) +\'%s' [%d.%d .. %d.%d], -\'%s', [%d.%d .. %d.%d] (@%d.%d)\n",
    //	aValue.mBefore.mCursorPosition.mLine, aValue.mBefore.mCursorPosition.mColumn,
    //	aValue.mAdded.c_str(), aValue.mAddedStart.mLine, aValue.mAddedStart.mColumn, aValue.mAddedEnd.mLine, aValue.mAddedEnd.mColumn,
    //	aValue.mRemoved.c_str(), aValue.mRemovedStart.mLine, aValue.mRemovedStart.mColumn, aValue.mRemovedEnd.mLine,
    //aValue.mRemovedEnd.mColumn, 	aValue.mAfter.mCursorPosition.mLine, aValue.mAfter.mCursorPosition.mColumn
    //	);

    mUndoBuffer.resize((size_t)(mUndoIndex + 1));
    mUndoBuffer.back() = aValue;
    ++mUndoIndex;
}

TextEditor::Coordinates TextEditor::ScreenPosToCoordinates(const ImVec2& aPosition) const {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 local(aPosition.x - origin.x, aPosition.y - origin.y);

    int lineNo = max(0, (int)floor(local.y / mCharAdvance.y));

    int columnCoord = 0;

    if (lineNo >= 0 && lineNo < (int)mLines.size()) {
        auto& line = mLines.at(lineNo);

        int columnIndex = 0;
        float columnX = 0.0f;

        while ((size_t)columnIndex < line.size()) {
            float columnWidth = 0.0f;

            if (line[columnIndex].mChar == '\t') {
                float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
                float oldX = columnX;
                float newColumnX = (1.0f + std::floor((1.0f + columnX) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
                columnWidth = newColumnX - oldX;
                if (mTextStart + columnX + columnWidth * 0.5f > local.x)
                    break;
                columnX = newColumnX;
                columnCoord = (columnCoord / mTabSize) * mTabSize + mTabSize;
                columnIndex++;
            } else {
                char buf[7];
                auto d = UTF8CharLength(line[columnIndex].mChar);
                int i = 0;
                while (i < 6 && d-- > 0)
                    buf[i++] = line[columnIndex++].mChar;
                buf[i] = '\0';
                columnWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf).x;
                if (mTextStart + columnX + columnWidth * 0.5f > local.x)
                    break;
                columnX += columnWidth;
                columnCoord++;
            }
        }
    }

    return SanitizeCoordinates(Coordinates(lineNo, columnCoord));
}

TextEditor::Coordinates TextEditor::FindWordStart(const Coordinates& aFrom) const {
    Coordinates at = aFrom;
    if (at.mLine >= (int)mLines.size())
        return at;

    auto& line = mLines[at.mLine];
    auto cindex = GetCharacterIndex(at);

    if (cindex >= (int)line.size())
        return at;

    while (cindex > 0 && isspace(line[cindex].mChar))
        --cindex;

    auto cstart = (PaletteIndex)line[cindex].mColorIndex;
    while (cindex > 0) {
        auto c = line[cindex].mChar;
        if ((c & 0xC0) != 0x80) // not UTF code sequence 10xxxxxx
        {
            if (c <= 32 && isspace(c)) {
                cindex++;
                break;
            }
            if (cstart != (PaletteIndex)line[size_t(cindex - 1)].mColorIndex)
                break;
        }
        --cindex;
    }
    return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindWordEnd(const Coordinates& aFrom) const {
    Coordinates at = aFrom;
    if (at.mLine >= (int)mLines.size())
        return at;

    auto& line = mLines[at.mLine];
    auto cindex = GetCharacterIndex(at);

    if (cindex >= (int)line.size())
        return at;

    bool prevspace = (bool)isspace(line[cindex].mChar);
    auto cstart = (PaletteIndex)line[cindex].mColorIndex;
    while (cindex < (int)line.size()) {
        auto c = line[cindex].mChar;
        auto d = UTF8CharLength(c);
        if (cstart != (PaletteIndex)line[cindex].mColorIndex)
            break;

        if (prevspace != !!isspace(c)) {
            if (isspace(c))
                while (cindex < (int)line.size() && isspace(line[cindex].mChar))
                    ++cindex;
            break;
        }
        cindex += d;
    }
    return Coordinates(aFrom.mLine, GetCharacterColumn(aFrom.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindNextWord(const Coordinates& aFrom) const {
    Coordinates at = aFrom;
    if (at.mLine >= (int)mLines.size())
        return at;

    // skip to the next non-word character
    auto cindex = GetCharacterIndex(aFrom);
    bool isword = false;
    bool skip = false;
    if (cindex < (int)mLines[at.mLine].size()) {
        auto& line = mLines[at.mLine];
        isword = isalnum(line[cindex].mChar);
        skip = isword;
    }

    while (!isword || skip) {
        if (at.mLine >= mLines.size()) {
            auto l = max(0, (int)mLines.size() - 1);
            return Coordinates(l, GetLineMaxColumn(l));
        }

        auto& line = mLines[at.mLine];
        if (cindex < (int)line.size()) {
            isword = isalnum(line[cindex].mChar);

            if (isword && !skip)
                return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));

            if (!isword)
                skip = false;

            cindex++;
        } else {
            cindex = 0;
            ++at.mLine;
            skip = false;
            isword = false;
        }
    }

    return at;
}

int TextEditor::GetCharacterIndex(const Coordinates& aCoordinates) const {
    if (aCoordinates.mLine >= mLines.size())
        return -1;
    auto& line = mLines[aCoordinates.mLine];
    int c = 0;
    int i = 0;
    for (; i < line.size() && c < aCoordinates.mColumn;) {
        if (line[i].mChar == '\t')
            c = (c / mTabSize) * mTabSize + mTabSize;
        else
            ++c;
        i += UTF8CharLength(line[i].mChar);
    }
    return i;
}

int TextEditor::GetCharacterColumn(int aLine, int aIndex) const {
    if (aLine >= mLines.size())
        return 0;
    auto& line = mLines[aLine];
    int col = 0;
    int i = 0;
    while (i < aIndex && i < (int)line.size()) {
        auto c = line[i].mChar;
        i += UTF8CharLength(c);
        if (c == '\t')
            col = (col / mTabSize) * mTabSize + mTabSize;
        else
            col++;
    }
    return col;
}

int TextEditor::GetLineCharacterCount(int aLine) const {
    if (aLine >= mLines.size())
        return 0;
    auto& line = mLines[aLine];
    int c = 0;
    for (unsigned i = 0; i < line.size(); c++)
        i += UTF8CharLength(line[i].mChar);
    return c;
}

int TextEditor::GetLineMaxColumn(int aLine) const {
    if (aLine >= mLines.size())
        return 0;
    auto& line = mLines[aLine];
    int col = 0;
    for (unsigned i = 0; i < line.size();) {
        auto c = line[i].mChar;
        if (c == '\t')
            col = (col / mTabSize) * mTabSize + mTabSize;
        else
            col++;
        i += UTF8CharLength(c);
    }
    return col;
}

bool TextEditor::IsOnWordBoundary(const Coordinates& aAt) const {
    if (aAt.mLine >= (int)mLines.size() || aAt.mColumn == 0)
        return true;

    auto& line = mLines[aAt.mLine];
    auto cindex = GetCharacterIndex(aAt);
    if (cindex >= (int)line.size())
        return true;

    if (mColorizerEnabled)
        return line[cindex].mColorIndex != line[size_t(cindex - 1)].mColorIndex;

    return isspace(line[cindex].mChar) != isspace(line[cindex - 1].mChar);
}

void TextEditor::RemoveLine(int aStart, int aEnd) {
    assert(!mReadOnly);
    assert(aEnd >= aStart);
    assert(mLines.size() > (size_t)(aEnd - aStart));

    ErrorMarkers etmp;
    for (auto& i : mErrorMarkers) {
        ErrorMarkers::value_type e(i.first >= aStart ? i.first - 1 : i.first, i.second);
        if (e.first >= aStart && e.first <= aEnd)
            continue;
        etmp.insert(e);
    }
    mErrorMarkers = std::move(etmp);

    Breakpoints btmp;
    for (auto i : mBreakpoints) {
        if (i >= aStart && i <= aEnd)
            continue;
        btmp.insert(i >= aStart ? i - 1 : i);
    }
    mBreakpoints = std::move(btmp);

    mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
    assert(!mLines.empty());

    mTextChanged = true;
}

void TextEditor::RemoveLine(int aIndex) {
    assert(!mReadOnly);
    assert(mLines.size() > 1);

    ErrorMarkers etmp;
    for (auto& i : mErrorMarkers) {
        ErrorMarkers::value_type e(i.first > aIndex ? i.first - 1 : i.first, i.second);
        if (e.first - 1 == aIndex)
            continue;
        etmp.insert(e);
    }
    mErrorMarkers = std::move(etmp);

    Breakpoints btmp;
    for (auto i : mBreakpoints) {
        if (i == aIndex)
            continue;
        btmp.insert(i >= aIndex ? i - 1 : i);
    }
    mBreakpoints = std::move(btmp);

    mLines.erase(mLines.begin() + aIndex);
    assert(!mLines.empty());

    mTextChanged = true;
}

TextEditor::Line& TextEditor::InsertLine(int aIndex) {
    assert(!mReadOnly);

    auto& result = *mLines.insert(mLines.begin() + aIndex, Line());

    ErrorMarkers etmp;
    for (auto& i : mErrorMarkers)
        etmp.insert(ErrorMarkers::value_type(i.first >= aIndex ? i.first + 1 : i.first, i.second));
    mErrorMarkers = std::move(etmp);

    Breakpoints btmp;
    for (auto i : mBreakpoints)
        btmp.insert(i >= aIndex ? i + 1 : i);
    mBreakpoints = std::move(btmp);

    return result;
}

std::string TextEditor::GetWordUnderCursor() const {
    auto c = GetCursorPosition();
    return GetWordAt(c);
}

std::string TextEditor::GetWordAt(const Coordinates& aCoords) const {
    auto start = FindWordStart(aCoords);
    auto end = FindWordEnd(aCoords);

    std::string r;

    auto istart = GetCharacterIndex(start);
    auto iend = GetCharacterIndex(end);

    for (auto it = istart; it < iend; ++it)
        r.push_back(mLines[aCoords.mLine][it].mChar);

    return r;
}

ImU32 TextEditor::GetGlyphColor(const Glyph& aGlyph) const {
    if (!mColorizerEnabled)
        return mPalette[(int)PaletteIndex::Default];
    if (aGlyph.mComment)
        return mPalette[(int)PaletteIndex::Comment];
    if (aGlyph.mMultiLineComment)
        return mPalette[(int)PaletteIndex::MultiLineComment];
    auto const color = mPalette[(int)aGlyph.mColorIndex];
    if (aGlyph.mPreprocessor) {
        const auto ppcolor = mPalette[(int)PaletteIndex::Preprocessor];
        const int c0 = ((ppcolor & 0xff) + (color & 0xff)) / 2;
        const int c1 = (((ppcolor >> 8) & 0xff) + ((color >> 8) & 0xff)) / 2;
        const int c2 = (((ppcolor >> 16) & 0xff) + ((color >> 16) & 0xff)) / 2;
        const int c3 = (((ppcolor >> 24) & 0xff) + ((color >> 24) & 0xff)) / 2;
        return ImU32(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24));
    }
    return color;
}

void TextEditor::HandleKeyboardInputs() {
    ImGuiIO& io = ImGui::GetIO();
    auto shift = io.KeyShift;
    auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsWindowHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        // ImGui::CaptureKeyboardFromApp(true);

        io.WantCaptureKeyboard = true;
        io.WantTextInput = true;

        if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Z))
            Undo();
        else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Y) || ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Z))
            Redo();
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            MoveUp(1, shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            MoveDown(1, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            MoveLeft(1, shift, ctrl);
        else if (!alt && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            MoveRight(1, shift, ctrl);
        else if (!alt && ImGui::IsKeyPressed(ImGuiKey_PageUp))
            MoveUp(GetPageSize() - 4, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGuiKey_PageDown))
            MoveDown(GetPageSize() - 4, shift);
        else if (!alt && ctrl && ImGui::IsKeyPressed(ImGuiKey_Home))
            MoveTop(shift);
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_End))
            MoveBottom(shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Home))
            MoveHome(shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_End))
            MoveEnd(shift);
        else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Delete))
            Delete();
        else if (!IsReadOnly() && ImGui::IsKeyPressed(ImGuiKey_Backspace))
            Backspace();
        else if (!ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
            mOverwrite ^= true;
        else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
            Copy();
        else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_C))
            Copy();
        else if (!IsReadOnly() && !ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
            Paste();
        else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_V))
            Paste();
        else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_X))
            Cut();
        else if (!ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Delete))
            Cut();
        else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_A))
            SelectAll();
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Slash))
          ToggleComment(shift);
        else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Enter))
            EnterCharacter('\n', false);
        else if (!IsReadOnly() && !ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            EnterCharacter(' ', false);
            EnterCharacter(' ', false);
        }

        if (!IsReadOnly() && !io.InputQueueCharacters.empty()) {
            for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                auto c = io.InputQueueCharacters[i];
                if (c != 0 && (c == '\n' || c >= 32))
                    EnterCharacter(c, shift);
            }
            io.InputQueueCharacters.resize(0);
        }
    }
}

void TextEditor::HandleMouseInputs() {
    ImGuiIO& io = ImGui::GetIO();
    auto shift = io.KeyShift;
    auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

    if (ImGui::IsWindowHovered()) {
        if (!shift && !alt) {
            auto click = ImGui::IsMouseClicked(0);
            auto doubleClick = ImGui::IsMouseDoubleClicked(0);
            auto t = ImGui::GetTime();
            auto tripleClick = click && !doubleClick && (mLastClick != -1.0f && (t - mLastClick) < io.MouseDoubleClickTime);

            /*
            Left mouse button triple click
            */

            if (tripleClick) {
                if (!ctrl) {
                    mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
                    mSelectionMode = SelectionMode::Line;
                    SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
                }

                mLastClick = -1.0f;
            }

            /*
            Left mouse button double click
            */

            else if (doubleClick) {
                if (!ctrl) {
                    mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
                    if (mSelectionMode == SelectionMode::Line)
                        mSelectionMode = SelectionMode::Normal;
                    else
                        mSelectionMode = SelectionMode::Word;
                    SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
                }

                mLastClick = (float)ImGui::GetTime();
            }

            /*
            Left mouse button click
            */
            else if (click) {
                mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
                if (ctrl)
                    mSelectionMode = SelectionMode::Word;
                else
                    mSelectionMode = SelectionMode::Normal;
                SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);

                mLastClick = (float)ImGui::GetTime();
            }
            // Mouse left button dragging (=> update selection)
            else if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0)) {
                io.WantCaptureMouse = true;
                mState.mCursorPosition = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
                SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
            }
        }
    }
}

void TextEditor::Render() {
    /* Compute mCharAdvance regarding to scaled font size (Ctrl + mouse wheel)*/
    const float fontSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr).x;
    mCharAdvance = ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * mLineSpacing);

    /* Update palette with the current alpha from style */
    for (int i = 0; i < (int)PaletteIndex::Max; ++i) {
        auto color = ImGui::ColorConvertU32ToFloat4(mPaletteBase[i]);
        color.w *= ImGui::GetStyle().Alpha;
        mPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
    }

    assert(mLineBuffer.empty());

    auto contentSize = ImGui::GetWindowContentRegionMax();
    auto drawList = ImGui::GetWindowDrawList();
    float longest(mTextStart);

    if (mScrollToTop) {
        mScrollToTop = false;
        ImGui::SetScrollY(0.f);
    }

    ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
    auto scrollX = ImGui::GetScrollX();
    auto scrollY = ImGui::GetScrollY();

    auto lineNo = (int)floor(scrollY / mCharAdvance.y);
    auto globalLineMax = (int)mLines.size();
    auto lineMax = max(0,min((int)mLines.size() - 1, lineNo + (int)floor((scrollY + contentSize.y) / mCharAdvance.y)));

    // Deduce mTextStart by evaluating mLines size (global lineMax) plus two spaces as text width
    char buf[16];
    snprintf(buf, 16, " %d ", globalLineMax);
    mTextStart = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x + mLeftMargin;

    if (!mLines.empty()) {
        float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

        while (lineNo <= lineMax) {
            ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * mCharAdvance.y);
            ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

            auto& line = mLines[lineNo];
            longest = max(mTextStart + TextDistanceToLineStart(Coordinates(lineNo, GetLineMaxColumn(lineNo))), longest);
            auto columnNo = 0;
            Coordinates lineStartCoord(lineNo, 0);
            Coordinates lineEndCoord(lineNo, GetLineMaxColumn(lineNo));

            // Draw selection for the current line
            float sstart = -1.0f;
            float ssend = -1.0f;

            assert(mState.mSelectionStart <= mState.mSelectionEnd);
            if (mState.mSelectionStart <= lineEndCoord)
                sstart = mState.mSelectionStart > lineStartCoord ? TextDistanceToLineStart(mState.mSelectionStart) : 0.0f;
            if (mState.mSelectionEnd > lineStartCoord)
                ssend = TextDistanceToLineStart(mState.mSelectionEnd < lineEndCoord ? mState.mSelectionEnd : lineEndCoord);

            if (mState.mSelectionEnd.mLine > lineNo)
                ssend += mCharAdvance.x;

            if (sstart != -1 && ssend != -1 && sstart < ssend) {
                ImVec2 vstart(lineStartScreenPos.x + mTextStart + sstart, lineStartScreenPos.y);
                ImVec2 vend(lineStartScreenPos.x + mTextStart + ssend, lineStartScreenPos.y + mCharAdvance.y);
                drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Selection]);
            }

            // Draw breakpoints
            auto start = ImVec2(lineStartScreenPos.x + scrollX, lineStartScreenPos.y);

            if (mBreakpoints.count(lineNo + 1) != 0) {
                auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
                drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::Breakpoint]);
            }

            // Draw error markers
            auto errorIt = mErrorMarkers.find(lineNo + 1);
            if (errorIt != mErrorMarkers.end()) {
                auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
                drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::ErrorMarker]);

                if (ImGui::IsMouseHoveringRect(lineStartScreenPos, end)) {
                    ImGui::BeginTooltip();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                    ImGui::Text("Error at line %d:", errorIt->first);
                    ImGui::PopStyleColor();
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
                    ImGui::Text("%s", errorIt->second.c_str());
                    ImGui::PopStyleColor();
                    ImGui::EndTooltip();
                }
            }

            // Draw line number (right aligned)
            snprintf(buf, 16, "%d  ", lineNo + 1);

            auto lineNoWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x;
            drawList->AddText(ImVec2(lineStartScreenPos.x + mTextStart - lineNoWidth, lineStartScreenPos.y),
                mPalette[(int)PaletteIndex::LineNumber], buf);

            if (mState.mCursorPosition.mLine == lineNo) {
                auto focused = ImGui::IsWindowFocused();

                // Highlight the current line (where the cursor is)
                if (!HasSelection()) {
                    auto end = ImVec2(start.x + contentSize.x + scrollX, start.y + mCharAdvance.y);
                    drawList->AddRectFilled(
                        start, end, mPalette[(int)(focused ? PaletteIndex::CurrentLineFill : PaletteIndex::CurrentLineFillInactive)]);
                    drawList->AddRect(start, end, mPalette[(int)PaletteIndex::CurrentLineEdge], 1.0f);
                }

                // Render the cursor
                if (focused) {
                    auto timeEnd =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    auto elapsed = timeEnd - mStartTime;
                    if (elapsed > 400) {
                        float width = 1.0f;
                        auto cindex = GetCharacterIndex(mState.mCursorPosition);
                        float cx = TextDistanceToLineStart(mState.mCursorPosition);

                        if (mOverwrite && cindex < (int)line.size()) {
                            auto c = line[cindex].mChar;
                            if (c == '\t') {
                                auto x = (1.0f + std::floor((1.0f + cx) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
                                width = x - cx;
                            } else {
                                char buf2[2];
                                buf2[0] = line[cindex].mChar;
                                buf2[1] = '\0';
                                width = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf2).x;
                            }
                        }
                        ImVec2 cstart(textScreenPos.x + cx, lineStartScreenPos.y);
                        ImVec2 cend(textScreenPos.x + cx + width, lineStartScreenPos.y + mCharAdvance.y);
                        drawList->AddRectFilled(cstart, cend, mPalette[(int)PaletteIndex::Cursor]);
                        if (elapsed > 800)
                            mStartTime = timeEnd;
                    }
                }
            }

            // Render colorized text
            auto prevColor = line.empty() ? mPalette[(int)PaletteIndex::Default] : GetGlyphColor(line[0]);
            ImVec2 bufferOffset;

            for (int i = 0; i < line.size();) {
                auto& glyph = line[i];
                auto color = GetGlyphColor(glyph);

                if ((color != prevColor || glyph.mChar == '\t' || glyph.mChar == ' ') && !mLineBuffer.empty()) {
                    const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
                    drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
                    auto textSize =
                        ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, mLineBuffer.c_str(), nullptr, nullptr);
                    bufferOffset.x += textSize.x;
                    mLineBuffer.clear();
                }
                prevColor = color;

                if (glyph.mChar == '\t') {
                    auto oldX = bufferOffset.x;
                    bufferOffset.x =
                        (1.0f + std::floor((1.0f + bufferOffset.x) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
                    ++i;

                    if (mShowWhitespaces) {
                        const auto s = ImGui::GetFontSize();
                        const auto x1 = textScreenPos.x + oldX + 1.0f;
                        const auto x2 = textScreenPos.x + bufferOffset.x - 1.0f;
                        const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
                        const ImVec2 p1(x1, y);
                        const ImVec2 p2(x2, y);
                        const ImVec2 p3(x2 - s * 0.2f, y - s * 0.2f);
                        const ImVec2 p4(x2 - s * 0.2f, y + s * 0.2f);
                        drawList->AddLine(p1, p2, 0x90909090);
                        drawList->AddLine(p2, p3, 0x90909090);
                        drawList->AddLine(p2, p4, 0x90909090);
                    }
                } else if (glyph.mChar == ' ') {
                    if (mShowWhitespaces) {
                        const auto s = ImGui::GetFontSize();
                        const auto x = textScreenPos.x + bufferOffset.x + spaceSize * 0.5f;
                        const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
                        drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
                    }
                    bufferOffset.x += spaceSize;
                    i++;
                } else {
                    auto l = UTF8CharLength(glyph.mChar);
                    while (l-- > 0)
                        mLineBuffer.push_back(line[i++].mChar);
                }
                ++columnNo;
            }

            if (!mLineBuffer.empty()) {
                const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
                drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
                mLineBuffer.clear();
            }

            ++lineNo;
        }

        // Draw a tooltip on known identifiers/preprocessor symbols
        if (ImGui::IsMousePosValid()) {
            auto id = GetWordAt(ScreenPosToCoordinates(ImGui::GetMousePos()));
            if (!id.empty()) {
                auto it = mLanguageDefinition.mIdentifiers.find(id);
                if (it != mLanguageDefinition.mIdentifiers.end()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(it->second.mDeclaration.c_str());
                    ImGui::EndTooltip();
                } else {
                    auto pi = mLanguageDefinition.mPreprocIdentifiers.find(id);
                    if (pi != mLanguageDefinition.mPreprocIdentifiers.end()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(pi->second.mDeclaration.c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsItemHovered())
        ImGui::OpenPopup("##context", ImGuiPopupFlags_MouseButtonRight);
    ImGui::SetNextWindowBgAlpha(0.8f);
    if (ImGui::BeginPopup("##context") || ImGui::BeginPopupContextItem()) {
        constexpr auto command = [](const char* name, const char* shortcut, bool selected, int shortcut_key = 0) -> bool {
            if (shortcut_key != 0 && ImGui::IsKeyPressed(static_cast<ImGuiKey>(shortcut_key), false) &&
                ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false))
                selected = true;
            bool item = ImGui::MenuItem(name, shortcut, &selected, true);
            return item;
        };
        auto selected = false;
        if (ImGui::IsItemClicked(0))
            ImGui::CloseCurrentPopup();
        if (command("Copy", "Ctrl-C", selected, ImGuiKey_C)) {
          Copy();
            ImGui::CloseCurrentPopup();
        } else if (command("Cut", "Ctrl-X", selected, ImGuiKey_X)) {
            Cut();
            ImGui::CloseCurrentPopup();
        } else if (command("Paste", "Ctrl-V", selected, ImGuiKey_V)) {
            Paste();
            ImGui::CloseCurrentPopup();
        } else if (command("Select All", "Ctrl-A", selected, ImGuiKey_A)) {
            SelectAll();
            ImGui::CloseCurrentPopup();
        } 
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2((longest + 2), mLines.size() * mCharAdvance.y));

    if (mScrollToCursor) {
        EnsureCursorVisible();
        ImGui::SetWindowFocus();
        mScrollToCursor = false;
    }
}

void TextEditor::Render(const char* aTitle, const ImVec2& aSize, bool aBorder) {
    mWithinRender = true;
    mTextChanged = false;
    mCursorPositionChanged = false;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (!mIgnoreImGuiChild)
        ImGui::BeginChild(aTitle, aSize, aBorder,
            ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoMove);

    if (mHandleKeyboardInputs) {
        HandleKeyboardInputs();
        ImGui::SetNextFrameWantCaptureKeyboard(true);
    }

    if (mHandleMouseInputs)
        HandleMouseInputs();

    ColorizeInternal();
    Render();

    if (mHandleKeyboardInputs)
        ImGui::SetNextFrameWantCaptureKeyboard(false);

    if (!mIgnoreImGuiChild)
        ImGui::EndChild();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    mWithinRender = false;
}

void TextEditor::SetText(const std::string& aText) {
    mLines.clear();
    mLines.emplace_back(Line());
    for (auto chr : aText) {
        if (chr == '\r') {
            // ignore the carriage return character
        } else if (chr == '\n')
            mLines.emplace_back(Line());
        else {
            mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
        }
    }

    mTextChanged = true;
    mScrollToTop = true;

    mUndoBuffer.clear();
    mUndoIndex = 0;

    Colorize();
}

void TextEditor::SetTextLines(const std::vector<std::string>& aLines) {
    mLines.clear();

    if (aLines.empty()) {
        mLines.emplace_back(Line());
    } else {
        mLines.resize(aLines.size());

        for (size_t i = 0; i < aLines.size(); ++i) {
            const std::string& aLine = aLines[i];

            mLines[i].reserve(aLine.size());
            for (size_t j = 0; j < aLine.size(); ++j)
                mLines[i].emplace_back(Glyph(aLine[j], PaletteIndex::Default));
        }
    }

    mTextChanged = true;
    mScrollToTop = true;

    mUndoBuffer.clear();
    mUndoIndex = 0;

    Colorize();
}

void TextEditor::EnterCharacter(ImWchar aChar, bool aShift) {
    assert(!mReadOnly);

    UndoRecord u;

    u.mBefore = mState;

    if (HasSelection()) {
        if (aChar == '\t' && mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine) {

            auto start = mState.mSelectionStart;
            auto end = mState.mSelectionEnd;
            auto originalEnd = end;

            if (start > end)
                std::swap(start, end);
            start.mColumn = 0;
            //			end.mColumn = end.mLine < mLines.size() ? mLines[end.mLine].size() : 0;
            if (end.mColumn == 0 && end.mLine > 0)
                --end.mLine;
            if (end.mLine >= (int)mLines.size())
                end.mLine = mLines.empty() ? 0 : (int)mLines.size() - 1;
            end.mColumn = GetLineMaxColumn(end.mLine);

            // if (end.mColumn >= GetLineMaxColumn(end.mLine))
            //	end.mColumn = GetLineMaxColumn(end.mLine) - 1;

            u.mRemovedStart = start;
            u.mRemovedEnd = end;
            u.mRemoved = GetText(start, end);

            bool modified = false;

            for (int i = start.mLine; i <= end.mLine; i++) {
                auto& line = mLines[i];
                if (aShift) {
                    if (!line.empty()) {
                        if (line.front().mChar == '\t') {
                            line.erase(line.begin());
                            modified = true;
                        } else {
                            for (int j = 0; j < mTabSize && !line.empty() && line.front().mChar == ' '; j++) {
                                line.erase(line.begin());
                                modified = true;
                            }
                        }
                    }
                } else {
                    line.insert(line.begin(), Glyph('\t', TextEditor::PaletteIndex::Background));
                    modified = true;
                }
            }

            if (modified) {
                start = Coordinates(start.mLine, GetCharacterColumn(start.mLine, 0));
                Coordinates rangeEnd;
                if (originalEnd.mColumn != 0) {
                    end = Coordinates(end.mLine, GetLineMaxColumn(end.mLine));
                    rangeEnd = end;
                    u.mAdded = GetText(start, end);
                } else {
                    end = Coordinates(originalEnd.mLine, 0);
                    rangeEnd = Coordinates(end.mLine - 1, GetLineMaxColumn(end.mLine - 1));
                    u.mAdded = GetText(start, rangeEnd);
                }

                u.mAddedStart = start;
                u.mAddedEnd = rangeEnd;
                u.mAfter = mState;

                mState.mSelectionStart = start;
                mState.mSelectionEnd = end;
                AddUndo(u);

                mTextChanged = true;

                EnsureCursorVisible();
            }

            return;
        } // c == '\t'
        else {
            u.mRemoved = GetSelectedText();
            u.mRemovedStart = mState.mSelectionStart;
            u.mRemovedEnd = mState.mSelectionEnd;
            DeleteSelection();
        }
    } // HasSelection

    auto coord = GetActualCursorCoordinates();
    u.mAddedStart = coord;

    assert(!mLines.empty());

    if (aChar == '\n') {
        InsertLine(coord.mLine + 1);
        auto& line = mLines[coord.mLine];
        auto& newLine = mLines[coord.mLine + 1];

        if (mLanguageDefinition.mAutoIndentation)
            for (size_t it = 0; it < line.size() && isascii(line[it].mChar) && isblank(line[it].mChar); ++it)
                newLine.push_back(line[it]);

        const size_t whitespaceSize = newLine.size();
        auto cindex = GetCharacterIndex(coord);
        newLine.insert(newLine.end(), line.begin() + cindex, line.end());
        line.erase(line.begin() + cindex, line.begin() + line.size());
        SetCursorPosition(Coordinates(coord.mLine + 1, GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)));
        u.mAdded = (char)aChar;
    } else {
        char buf[7];
        int e = ImTextCharToUtf8(buf, 7, aChar);
        if (e > 0) {
            buf[e] = '\0';
            auto& line = mLines[coord.mLine];
            auto cindex = GetCharacterIndex(coord);

            if (mOverwrite && cindex < (int)line.size()) {
                auto d = UTF8CharLength(line[cindex].mChar);

                u.mRemovedStart = mState.mCursorPosition;
                u.mRemovedEnd = Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex + d));

                while (d-- > 0 && cindex < (int)line.size()) {
                    u.mRemoved += line[cindex].mChar;
                    line.erase(line.begin() + cindex);
                }
            }

            for (auto p = buf; *p != '\0'; p++, ++cindex)
                line.insert(line.begin() + cindex, Glyph(*p, PaletteIndex::Default));
            u.mAdded = buf;

            SetCursorPosition(Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)));
        } else
            return;
    }

    mTextChanged = true;

    u.mAddedEnd = GetActualCursorCoordinates();
    u.mAfter = mState;

    AddUndo(u);

    Colorize(coord.mLine - 1, 3);
    EnsureCursorVisible();
}

void TextEditor::SetReadOnly(bool aValue) {
    mReadOnly = aValue;
}

void TextEditor::SetColorizerEnable(bool aValue) {
    mColorizerEnabled = aValue;
}

void TextEditor::SetCursorPosition(const Coordinates& aPosition) {
    if (mState.mCursorPosition != aPosition) {
        mState.mCursorPosition = aPosition;
        mCursorPositionChanged = true;
        EnsureCursorVisible();
    }
}

void TextEditor::SetSelectionStart(const Coordinates& aPosition) {
    mState.mSelectionStart = SanitizeCoordinates(aPosition);
    if (mState.mSelectionStart > mState.mSelectionEnd)
        std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void TextEditor::SetSelectionEnd(const Coordinates& aPosition) {
    mState.mSelectionEnd = SanitizeCoordinates(aPosition);
    if (mState.mSelectionStart > mState.mSelectionEnd)
        std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void TextEditor::SetSelection(const Coordinates& aStart, const Coordinates& aEnd, SelectionMode aMode) {
    auto oldSelStart = mState.mSelectionStart;
    auto oldSelEnd = mState.mSelectionEnd;

    mState.mSelectionStart = SanitizeCoordinates(aStart);
    mState.mSelectionEnd = SanitizeCoordinates(aEnd);
    if (mState.mSelectionStart > mState.mSelectionEnd)
        std::swap(mState.mSelectionStart, mState.mSelectionEnd);

    switch (aMode) {
    case TextEditor::SelectionMode::Normal:
        break;
    case TextEditor::SelectionMode::Word: {
        mState.mSelectionStart = FindWordStart(mState.mSelectionStart);
        if (!IsOnWordBoundary(mState.mSelectionEnd))
            mState.mSelectionEnd = FindWordEnd(FindWordStart(mState.mSelectionEnd));
        break;
    }
    case TextEditor::SelectionMode::Line: {
        const auto lineNo = mState.mSelectionEnd.mLine;
        const auto lineSize = (size_t)lineNo < mLines.size() ? mLines[lineNo].size() : 0;
        mState.mSelectionStart = Coordinates(mState.mSelectionStart.mLine, 0);
        mState.mSelectionEnd = Coordinates(lineNo, GetLineMaxColumn(lineNo));
        break;
    }
    default:
        break;
    }

    if (mState.mSelectionStart != oldSelStart || mState.mSelectionEnd != oldSelEnd)
        mCursorPositionChanged = true;
}

void TextEditor::SetTabSize(int aValue) {
    mTabSize = max(0, min(32, aValue));
}

void TextEditor::InsertText(const std::string& aValue) {
    InsertText(aValue.c_str());
}

void TextEditor::InsertText(const char* aValue) {
    if (aValue == nullptr)
        return;

    auto pos = GetActualCursorCoordinates();
    auto start = min(pos, mState.mSelectionStart);
    int totalLines = pos.mLine - start.mLine;

    totalLines += InsertTextAt(pos, aValue);

    SetSelection(pos, pos);
    SetCursorPosition(pos);
    Colorize(start.mLine - 1, totalLines + 2);
}

void TextEditor::DeleteSelection() {
    assert(mState.mSelectionEnd >= mState.mSelectionStart);

    if (mState.mSelectionEnd == mState.mSelectionStart)
        return;

    DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);

    SetSelection(mState.mSelectionStart, mState.mSelectionStart);
    SetCursorPosition(mState.mSelectionStart);
    Colorize(mState.mSelectionStart.mLine, 1);
}

void TextEditor::MoveUp(int aAmount, bool aSelect) {
    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition.mLine = max(0, mState.mCursorPosition.mLine - aAmount);
    if (oldPos != mState.mCursorPosition) {
        if (aSelect) {
            if (oldPos == mInteractiveStart)
                mInteractiveStart = mState.mCursorPosition;
            else if (oldPos == mInteractiveEnd)
                mInteractiveEnd = mState.mCursorPosition;
            else {
                mInteractiveStart = mState.mCursorPosition;
                mInteractiveEnd = oldPos;
            }
        } else
            mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
        SetSelection(mInteractiveStart, mInteractiveEnd);

        EnsureCursorVisible();
    }
}

void TextEditor::MoveDown(int aAmount, bool aSelect) {
    assert(mState.mCursorPosition.mColumn >= 0);
    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition.mLine = max(0, min((int)mLines.size() - 1, mState.mCursorPosition.mLine + aAmount));

    if (mState.mCursorPosition != oldPos) {
        if (aSelect) {
            if (oldPos == mInteractiveEnd)
                mInteractiveEnd = mState.mCursorPosition;
            else if (oldPos == mInteractiveStart)
                mInteractiveStart = mState.mCursorPosition;
            else {
                mInteractiveStart = oldPos;
                mInteractiveEnd = mState.mCursorPosition;
            }
        } else
            mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
        SetSelection(mInteractiveStart, mInteractiveEnd);

        EnsureCursorVisible();
    }
}

static bool IsUTFSequence(char c) {
    return (c & 0xC0) == 0x80;
}

void TextEditor::MoveLeft(int aAmount, bool aSelect, bool aWordMode) {
    if (mLines.empty())
        return;

    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition = GetActualCursorCoordinates();
    auto line = mState.mCursorPosition.mLine;
    auto cindex = GetCharacterIndex(mState.mCursorPosition);

    while (aAmount-- > 0) {
        if (cindex == 0) {
            if (line > 0) {
                --line;
                if ((int)mLines.size() > line)
                    cindex = (int)mLines[line].size();
                else
                    cindex = 0;
            }
        } else {
            --cindex;
            if (cindex > 0) {
                if ((int)mLines.size() > line) {
                    while (cindex > 0 && IsUTFSequence(mLines[line][cindex].mChar))
                        --cindex;
                }
            }
        }

        mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));
        if (aWordMode) {
            mState.mCursorPosition = FindWordStart(mState.mCursorPosition);
            cindex = GetCharacterIndex(mState.mCursorPosition);
        }
    }

    mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));

    assert(mState.mCursorPosition.mColumn >= 0);
    if (aSelect) {
        if (oldPos == mInteractiveStart)
            mInteractiveStart = mState.mCursorPosition;
        else if (oldPos == mInteractiveEnd)
            mInteractiveEnd = mState.mCursorPosition;
        else {
            mInteractiveStart = mState.mCursorPosition;
            mInteractiveEnd = oldPos;
        }
    } else
        mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

    EnsureCursorVisible();
}

void TextEditor::MoveRight(int aAmount, bool aSelect, bool aWordMode) {
    auto oldPos = mState.mCursorPosition;

    if (mLines.empty() || oldPos.mLine >= mLines.size())
        return;

    auto cindex = GetCharacterIndex(mState.mCursorPosition);
    while (aAmount-- > 0) {
        auto lindex = mState.mCursorPosition.mLine;
        auto& line = mLines[lindex];

        if (cindex >= line.size()) {
            if (mState.mCursorPosition.mLine < mLines.size() - 1) {
                mState.mCursorPosition.mLine = max(0, min((int)mLines.size() - 1, mState.mCursorPosition.mLine + 1));
                mState.mCursorPosition.mColumn = 0;
            } else
                return;
        } else {
            cindex += UTF8CharLength(line[cindex].mChar);
            mState.mCursorPosition = Coordinates(lindex, GetCharacterColumn(lindex, cindex));
            if (aWordMode)
                mState.mCursorPosition = FindNextWord(mState.mCursorPosition);
        }
    }

    if (aSelect) {
        if (oldPos == mInteractiveEnd)
            mInteractiveEnd = SanitizeCoordinates(mState.mCursorPosition);
        else if (oldPos == mInteractiveStart)
            mInteractiveStart = mState.mCursorPosition;
        else {
            mInteractiveStart = oldPos;
            mInteractiveEnd = mState.mCursorPosition;
        }
    } else
        mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
    SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

    EnsureCursorVisible();
}

void TextEditor::MoveTop(bool aSelect) {
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(0, 0));

    if (mState.mCursorPosition != oldPos) {
        if (aSelect) {
            mInteractiveEnd = oldPos;
            mInteractiveStart = mState.mCursorPosition;
        } else
            mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
        SetSelection(mInteractiveStart, mInteractiveEnd);
    }
}

void TextEditor::TextEditor::MoveBottom(bool aSelect) {
    auto oldPos = GetCursorPosition();
    auto newPos = Coordinates((int)mLines.size() - 1, 0);
    SetCursorPosition(newPos);
    if (aSelect) {
        mInteractiveStart = oldPos;
        mInteractiveEnd = newPos;
    } else
        mInteractiveStart = mInteractiveEnd = newPos;
    SetSelection(mInteractiveStart, mInteractiveEnd);
}

void TextEditor::MoveHome(bool aSelect) {
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, 0));

    if (mState.mCursorPosition != oldPos) {
        if (aSelect) {
            if (oldPos == mInteractiveStart)
                mInteractiveStart = mState.mCursorPosition;
            else if (oldPos == mInteractiveEnd)
                mInteractiveEnd = mState.mCursorPosition;
            else {
                mInteractiveStart = mState.mCursorPosition;
                mInteractiveEnd = oldPos;
            }
        } else
            mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
        SetSelection(mInteractiveStart, mInteractiveEnd);
    }
}

void TextEditor::MoveEnd(bool aSelect) {
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, GetLineMaxColumn(oldPos.mLine)));

    if (mState.mCursorPosition != oldPos) {
        if (aSelect) {
            if (oldPos == mInteractiveEnd)
                mInteractiveEnd = mState.mCursorPosition;
            else if (oldPos == mInteractiveStart)
                mInteractiveStart = mState.mCursorPosition;
            else {
                mInteractiveStart = oldPos;
                mInteractiveEnd = mState.mCursorPosition;
            }
        } else
            mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
        SetSelection(mInteractiveStart, mInteractiveEnd);
    }
}

void TextEditor::ToggleComment(bool shift) {
    // Determine start and end lines
    size_t start_line = (size_t)mState.mCursorPosition.mLine;
    size_t end_line = start_line;

    if (mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine) {
        start_line = (size_t)min(mState.mSelectionStart.mLine, mState.mSelectionEnd.mLine);
        end_line = (size_t)max(mState.mSelectionStart.mLine, mState.mSelectionEnd.mLine);
    }

    // Nothing to do for a single empty line
    if (start_line < mLines.size() && mLines[start_line].empty() && end_line == start_line)
        return;

    UndoRecord u;
    u.mBefore = mState;

    // Find first non-whitespace char index in the first line
    size_t first_non_ws = 0;
    if (start_line < mLines.size()) {
        const auto& firstLine = mLines[start_line];
        while (first_non_ws < firstLine.size() && std::isspace(static_cast<unsigned char>(firstLine[first_non_ws].mChar)))
            ++first_non_ws;
    }

    if (shift) {
        // Block comment toggle
        if (!HasSelection()) {
            // Insert /* */ at cursor
            auto coord = GetActualCursorCoordinates();
            Coordinates insertPos = coord;
            const char* block = "/* */";
            InsertTextAt(insertPos, block);
            // Place cursor between the comment markers (after "/*")
            SetCursorPosition(Coordinates(coord.mLine, coord.mColumn + 2));

            u.mAdded = block;
            u.mAddedStart = coord;
            u.mAddedEnd = Coordinates(coord.mLine, coord.mColumn + 4);
            u.mAfter = mState;
            AddUndo(u);
            return;
        } else {
            // Wrap or unwrap selection
            auto selStart = SanitizeCoordinates(mState.mSelectionStart);
            auto selEnd = SanitizeCoordinates(mState.mSelectionEnd);
            if (selStart > selEnd)
                std::swap(selStart, selEnd);

            std::string selText = GetText(selStart, selEnd);
            u.mRemoved = selText;
            u.mRemovedStart = selStart;
            u.mRemovedEnd = selEnd;

            bool isWrapped = selText.size() >= 4 && selText.substr(0, 2) == "/*" && selText.substr(selText.size() - 2) == "*/";

            if (isWrapped) {
                // Unwrap: remove /* and */ from selection
                std::string newText = selText.substr(2, selText.size() - 4);
                DeleteRange(selStart, selEnd);
                auto insertPos = selStart;
                InsertTextAt(insertPos, newText.c_str());

                u.mAdded = newText;
                u.mAddedStart = selStart;
                u.mAddedEnd = insertPos;
                mState.mSelectionStart = selStart;
                mState.mSelectionEnd = insertPos;
                u.mAfter = mState;
                AddUndo(u);
                return;
            } else {
                // Wrap: replace selection with /* selection */
                std::string newText = "/*" + selText + "*/";
                DeleteRange(selStart, selEnd);
                auto insertPos = selStart;
                InsertTextAt(insertPos, newText.c_str());

                u.mAdded = newText;
                u.mAddedStart = selStart;
                u.mAddedEnd = insertPos;
                // Adjust selection to cover newly inserted block
                mState.mSelectionStart = selStart;
                mState.mSelectionEnd = insertPos;
                u.mAfter = mState;
                AddUndo(u);
                return;
            }
        }
    } else {
        // Single-line comment mode: operate on each line in range
        bool uncomment_all = false;

        // Look at first line to see if it begins with //
        if (start_line < mLines.size()) {
            const auto& fl = mLines[start_line];
            if (first_non_ws + 1 < fl.size() && fl[first_non_ws].mChar == '/' && fl[first_non_ws + 1].mChar == '/')
                uncomment_all = true;
        }

        // If we think we can uncomment all, verify every line is commented (after whitespace)
        if (uncomment_all) {
            for (size_t line_idx = start_line; line_idx <= end_line && line_idx < mLines.size(); ++line_idx) {
                const auto& line = mLines[line_idx];
                if (line.empty())
                    continue;
                size_t non_ws = 0;
                while (non_ws < line.size() && std::isspace(static_cast<unsigned char>(line[non_ws].mChar)))
                    ++non_ws;
                if (!(non_ws + 1 < line.size() && line[non_ws].mChar == '/' && line[non_ws + 1].mChar == '/')) {
                    uncomment_all = false;
                    break;
                }
            }
        }

        // Perform uncomment or comment
        bool didModify = false;
        for (size_t line_idx = start_line; line_idx <= end_line && line_idx < mLines.size(); ++line_idx) {
            auto& line = mLines[line_idx];
            if (line.empty())
                continue;

            // Find first non-whitespace character
            size_t non_ws = 0;
            while (non_ws < line.size() && std::isspace(static_cast<unsigned char>(line[non_ws].mChar)))
                ++non_ws;

            if (uncomment_all) {
                if (non_ws + 1 < line.size() && line[non_ws].mChar == '/' && line[non_ws + 1].mChar == '/') {
                    line.erase(line.begin() + non_ws, line.begin() + non_ws + 2);
                    didModify = true;

                    // Adjust cursor and selection columns if necessary
                    if (mState.mCursorPosition.mLine == (int)line_idx && mState.mCursorPosition.mColumn > (int)non_ws)
                        mState.mCursorPosition.mColumn = max((int)non_ws, mState.mCursorPosition.mColumn - 2);
                    if (mState.mSelectionStart.mLine == (int)line_idx && mState.mSelectionStart.mColumn > (int)non_ws)
                        mState.mSelectionStart.mColumn = max((int)non_ws, mState.mSelectionStart.mColumn - 2);
                    if (mState.mSelectionEnd.mLine == (int)line_idx && mState.mSelectionEnd.mColumn > (int)non_ws)
                        mState.mSelectionEnd.mColumn = max((int)non_ws, mState.mSelectionEnd.mColumn - 2);
                }
            } else {
                // Insert "//" at non_ws
                Glyph slash('/', PaletteIndex::Comment);
                line.insert(line.begin() + non_ws, slash);
                line.insert(line.begin() + non_ws + 1, slash);
                didModify = true;

                if (mState.mCursorPosition.mLine == (int)line_idx && mState.mCursorPosition.mColumn >= (int)non_ws)
                    mState.mCursorPosition.mColumn += 2;
                if (mState.mSelectionStart.mLine == (int)line_idx && mState.mSelectionStart.mColumn >= (int)non_ws)
                    mState.mSelectionStart.mColumn += 2;
                if (mState.mSelectionEnd.mLine == (int)line_idx && mState.mSelectionEnd.mColumn >= (int)non_ws)
                    mState.mSelectionEnd.mColumn += 2;
            }
        }

        if (didModify) {
            // Best-effort undo information: record the affected text region
            Coordinates beg = Coordinates((int)start_line, 0);
            Coordinates end = Coordinates((int)end_line, GetLineMaxColumn((int)end_line));
            u.mAdded = GetText(beg, end);
            u.mAddedStart = beg;
            u.mAddedEnd = end;
            u.mAfter = mState;
            AddUndo(u);
        }
    }

    // Reset cursor animation (keep consistent with rest of editor)
    mStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    // Trigger syntax highlighting update for the affected lines
    mColorRangeMin = min(mColorRangeMin, (int)start_line);
    mColorRangeMax = max(mColorRangeMax, (int)end_line + 1);
}

void TextEditor::Delete() {
    assert(!mReadOnly);

    if (mLines.empty())
        return;

    UndoRecord u;
    u.mBefore = mState;

    if (HasSelection()) {
        u.mRemoved = GetSelectedText();
        u.mRemovedStart = mState.mSelectionStart;
        u.mRemovedEnd = mState.mSelectionEnd;

        DeleteSelection();
    } else {
        auto pos = GetActualCursorCoordinates();
        SetCursorPosition(pos);
        auto& line = mLines[pos.mLine];

        if (pos.mColumn == GetLineMaxColumn(pos.mLine)) {
            if (pos.mLine == (int)mLines.size() - 1)
                return;

            u.mRemoved = '\n';
            u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
            Advance(u.mRemovedEnd);

            auto& nextLine = mLines[pos.mLine + 1];
            line.insert(line.end(), nextLine.begin(), nextLine.end());
            RemoveLine(pos.mLine + 1);
        } else {
            auto cindex = GetCharacterIndex(pos);
            u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
            u.mRemovedEnd.mColumn++;
            u.mRemoved = GetText(u.mRemovedStart, u.mRemovedEnd);

            auto d = UTF8CharLength(line[cindex].mChar);
            while (d-- > 0 && cindex < (int)line.size())
                line.erase(line.begin() + cindex);
        }

        mTextChanged = true;

        Colorize(pos.mLine, 1);
    }

    u.mAfter = mState;
    AddUndo(u);
}

void TextEditor::Backspace() {
    assert(!mReadOnly);

    if (mLines.empty())
        return;

    UndoRecord u;
    u.mBefore = mState;

    if (HasSelection()) {
        u.mRemoved = GetSelectedText();
        u.mRemovedStart = mState.mSelectionStart;
        u.mRemovedEnd = mState.mSelectionEnd;

        DeleteSelection();
    } else {
        auto pos = GetActualCursorCoordinates();
        SetCursorPosition(pos);

        if (mState.mCursorPosition.mColumn == 0) {
            if (mState.mCursorPosition.mLine == 0)
                return;

            u.mRemoved = '\n';
            u.mRemovedStart = u.mRemovedEnd = Coordinates(pos.mLine - 1, GetLineMaxColumn(pos.mLine - 1));
            Advance(u.mRemovedEnd);

            auto& line = mLines[mState.mCursorPosition.mLine];
            auto& prevLine = mLines[mState.mCursorPosition.mLine - 1];
            auto prevSize = GetLineMaxColumn(mState.mCursorPosition.mLine - 1);
            prevLine.insert(prevLine.end(), line.begin(), line.end());

            ErrorMarkers etmp;
            for (auto& i : mErrorMarkers)
                etmp.insert(ErrorMarkers::value_type(i.first - 1 == mState.mCursorPosition.mLine ? i.first - 1 : i.first, i.second));
            mErrorMarkers = std::move(etmp);

            RemoveLine(mState.mCursorPosition.mLine);
            --mState.mCursorPosition.mLine;
            mState.mCursorPosition.mColumn = prevSize;
        } else {
            auto& line = mLines[mState.mCursorPosition.mLine];
            auto cindex = GetCharacterIndex(pos) - 1;
            auto cend = cindex + 1;
            while (cindex > 0 && IsUTFSequence(line[cindex].mChar))
                --cindex;

            // if (cindex > 0 && UTF8CharLength(line[cindex].mChar) > 1)
            //	--cindex;

            u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
            --u.mRemovedStart.mColumn;
            --mState.mCursorPosition.mColumn;

            while (cindex < line.size() && cend-- > cindex) {
                u.mRemoved += line[cindex].mChar;
                line.erase(line.begin() + cindex);
            }
        }

        mTextChanged = true;

        EnsureCursorVisible();
        Colorize(mState.mCursorPosition.mLine, 1);
    }

    u.mAfter = mState;
    AddUndo(u);
}

void TextEditor::SelectWordUnderCursor() {
    auto c = GetCursorPosition();
    SetSelection(FindWordStart(c), FindWordEnd(c));
}

void TextEditor::SelectAll() {
    SetSelection(Coordinates(0, 0), Coordinates((int)mLines.size(), 0));
}

bool TextEditor::HasSelection() const {
    return mState.mSelectionEnd > mState.mSelectionStart;
}

void TextEditor::Copy() {
    if (HasSelection()) {
        ImGui::SetClipboardText(GetSelectedText().c_str());
    } else {
        if (!mLines.empty()) {
            std::string str;
            auto& line = mLines[GetActualCursorCoordinates().mLine];
            for (auto& g : line)
                str.push_back(g.mChar);
            ImGui::SetClipboardText(str.c_str());
        }
    }
}

void TextEditor::Cut() {
    if (IsReadOnly()) {
        Copy();
    } else {
        if (HasSelection()) {
            UndoRecord u;
            u.mBefore = mState;
            u.mRemoved = GetSelectedText();
            u.mRemovedStart = mState.mSelectionStart;
            u.mRemovedEnd = mState.mSelectionEnd;

            Copy();
            DeleteSelection();

            u.mAfter = mState;
            AddUndo(u);
        }
    }
}

void TextEditor::Paste() {
    if (IsReadOnly())
        return;

    auto clipText = ImGui::GetClipboardText();
    if (clipText != nullptr && strlen(clipText) > 0) {
        UndoRecord u;
        u.mBefore = mState;

        if (HasSelection()) {
            u.mRemoved = GetSelectedText();
            u.mRemovedStart = mState.mSelectionStart;
            u.mRemovedEnd = mState.mSelectionEnd;
            DeleteSelection();
        }

        u.mAdded = clipText;
        u.mAddedStart = GetActualCursorCoordinates();

        InsertText(clipText);

        u.mAddedEnd = GetActualCursorCoordinates();
        u.mAfter = mState;
        AddUndo(u);
    }
}

bool TextEditor::CanUndo() const {
    return !mReadOnly && mUndoIndex > 0;
}

bool TextEditor::CanRedo() const {
    return !mReadOnly && mUndoIndex < (int)mUndoBuffer.size();
}

void TextEditor::Undo(int aSteps) {
    while (CanUndo() && aSteps-- > 0)
        mUndoBuffer[--mUndoIndex].Undo(this);
}

void TextEditor::Redo(int aSteps) {
    while (CanRedo() && aSteps-- > 0)
        mUndoBuffer[mUndoIndex++].Redo(this);
}

const TextEditor::Palette& TextEditor::GetDarkPalette() {
    const static Palette p = {{
        ImColor{204, 204, 204, 255}, // Default
        ImColor{249, 117, 131, 255}, // Keyword
        ImColor{248, 248, 248, 255}, // Number
        ImColor{255, 171, 112, 255}, // String
        ImColor{255, 171, 112, 255}, // Char literal
        ImColor{204, 204, 204, 255}, // Punctuation
        0xff408080,                  // Preprocessor
        ImColor{204, 204, 204, 255}, // Identifier
        ImColor{179, 146, 240, 255}, // Known identifier
        0xffc040a0,                  // Preproc identifier
        ImColor{107, 115, 124, 255}, // Comment (single line)
        ImColor{107, 115, 124, 255}, // Comment (multi line)
        ImColor{31, 31, 31, 255},    // Background
        0xffe0e0e0,                  // Cursor
        0x80a06020,                  // Selection
        0x800020ff,                  // ErrorMarker
        0x40f08000,                  // Breakpoint
        ImColor{114, 114, 114, 255}, // Line number
        ImColor{48, 48, 48, 255},    // Current line fill
        ImColor{48, 48, 48, 255},    // Current line fill (inactive)
        0x40a0a0a0,                  // Current line edge
    }};
    return p;
}

const TextEditor::Palette& TextEditor::GetLightPalette() {
    const static Palette p = {{
        0xff7f7f7f, // None
        0xffff0c06, // Keyword
        0xff008000, // Number
        0xff2020a0, // String
        0xff304070, // Char literal
        0xff000000, // Punctuation
        0xff406060, // Preprocessor
        0xff404040, // Identifier
        0xff606010, // Known identifier
        0xffc040a0, // Preproc identifier
        0xff205020, // Comment (single line)
        0xff405020, // Comment (multi line)
        0xffffffff, // Background
        0xff000000, // Cursor
        0x80600000, // Selection
        0xa00010ff, // ErrorMarker
        0x80f08000, // Breakpoint
        0xff505000, // Line number
        0x40000000, // Current line fill
        0x40808080, // Current line fill (inactive)
        0x40000000, // Current line edge
    }};
    return p;
}

const TextEditor::Palette& TextEditor::GetRetroBluePalette() {
    const static Palette p = {{
        0xff00ffff, // None
        0xffffff00, // Keyword
        0xff00ff00, // Number
        0xff808000, // String
        0xff808000, // Char literal
        0xffffffff, // Punctuation
        0xff008000, // Preprocessor
        0xff00ffff, // Identifier
        0xffffffff, // Known identifier
        0xffff00ff, // Preproc identifier
        0xff808080, // Comment (single line)
        0xff404040, // Comment (multi line)
        0xff800000, // Background
        0xff0080ff, // Cursor
        0x80ffff00, // Selection
        0xa00000ff, // ErrorMarker
        0x80ff8000, // Breakpoint
        0xff808000, // Line number
        0x40000000, // Current line fill
        0x40808080, // Current line fill (inactive)
        0x40000000, // Current line edge
    }};
    return p;
}

std::string TextEditor::GetText() const {
    return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::vector<std::string> TextEditor::GetTextLines() const {
    std::vector<std::string> result;

    result.reserve(mLines.size());

    for (auto& line : mLines) {
        std::string text;

        text.resize(line.size());

        for (size_t i = 0; i < line.size(); ++i)
            text[i] = line[i].mChar;

        result.emplace_back(std::move(text));
    }

    return result;
}

std::string TextEditor::GetSelectedText() const {
    return GetText(mState.mSelectionStart, mState.mSelectionEnd);
}

std::string TextEditor::GetCurrentLineText() const {
    auto lineLength = GetLineMaxColumn(mState.mCursorPosition.mLine);
    return GetText(Coordinates(mState.mCursorPosition.mLine, 0), Coordinates(mState.mCursorPosition.mLine, lineLength));
}

void TextEditor::ProcessInputs() {
}

void TextEditor::Colorize(int aFromLine, int aLines) {
    int toLine = aLines == -1 ? (int)mLines.size() : min((int)mLines.size(), aFromLine + aLines);
    mColorRangeMin = min(mColorRangeMin, aFromLine);
    mColorRangeMax = max(mColorRangeMax, toLine);
    mColorRangeMin = max(0, mColorRangeMin);
    mColorRangeMax = max(mColorRangeMin, mColorRangeMax);
    mCheckComments = true;
}

void TextEditor::ColorizeRange(int aFromLine, int aToLine) {
    if (mLines.empty() || aFromLine >= aToLine)
        return;

    std::string buffer;
    std::cmatch results;
    std::string id;

    int endLine = max(0, min((int)mLines.size(), aToLine));
    for (int i = aFromLine; i < endLine; ++i) {
        auto& line = mLines[i];

        if (line.empty())
            continue;

        buffer.resize(line.size());
        for (size_t j = 0; j < line.size(); ++j) {
            auto& col = line[j];
            buffer[j] = col.mChar;
            col.mColorIndex = PaletteIndex::Default;
        }

        const char* bufferBegin = &buffer.front();
        const char* bufferEnd = bufferBegin + buffer.size();

        auto last = bufferEnd;

        for (auto first = bufferBegin; first != last;) {
            const char* token_begin = nullptr;
            const char* token_end = nullptr;
            PaletteIndex token_color = PaletteIndex::Default;

            bool hasTokenizeResult = false;

            if (mLanguageDefinition.mTokenize != nullptr) {
                if (mLanguageDefinition.mTokenize(first, last, token_begin, token_end, token_color))
                    hasTokenizeResult = true;
            }

            if (hasTokenizeResult == false) {
                // todo : remove
                // printf("using regex for %.*s\n", first + 10 < last ? 10 : int(last - first), first);

                for (auto& p : mRegexList) {
                    if (std::regex_search(first, last, results, p.first, std::regex_constants::match_continuous)) {
                        hasTokenizeResult = true;

                        auto& v = *results.begin();
                        token_begin = v.first;
                        token_end = v.second;
                        token_color = p.second;
                        break;
                    }
                }
            }

            if (hasTokenizeResult == false) {
                first++;
            } else {
                const size_t token_length = token_end - token_begin;

                if (token_color == PaletteIndex::Identifier) {
                    id.assign(token_begin, token_end);

                    // todo : allmost all language definitions use lower case to specify keywords, so shouldn't this use ::tolower ?
                    if (!mLanguageDefinition.mCaseSensitive)
                        std::transform(id.begin(), id.end(), id.begin(), ::toupper);

                    if (!line[first - bufferBegin].mPreprocessor) {
                        if (mLanguageDefinition.mKeywords.count(id) != 0)
                            token_color = PaletteIndex::Keyword;
                        else if (mLanguageDefinition.mIdentifiers.count(id) != 0)
                            token_color = PaletteIndex::KnownIdentifier;
                        else if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
                            token_color = PaletteIndex::PreprocIdentifier;
                    } else {
                        if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
                            token_color = PaletteIndex::PreprocIdentifier;
                    }
                }

                for (size_t j = 0; j < token_length; ++j)
                    line[(token_begin - bufferBegin) + j].mColorIndex = token_color;

                first = token_end;
            }
        }
    }
}

void TextEditor::ColorizeInternal() {
    if (mLines.empty() || !mColorizerEnabled)
        return;

    if (mCheckComments) {
        auto endLine = mLines.size();
        auto endIndex = 0;
        auto commentStartLine = endLine;
        auto commentStartIndex = endIndex;
        auto withinString = false;
        auto withinSingleLineComment = false;
        auto withinPreproc = false;
        auto firstChar = true;    // there is no other non-whitespace characters in the line before
        auto concatenate = false; // '\' on the very end of the line
        auto currentLine = 0;
        auto currentIndex = 0;
        while (currentLine < endLine || currentIndex < endIndex) {
            auto& line = mLines[currentLine];

            if (currentIndex == 0 && !concatenate) {
                withinSingleLineComment = false;
                withinPreproc = false;
                firstChar = true;
            }

            concatenate = false;

            if (!line.empty()) {
                auto& g = line[currentIndex];
                auto c = g.mChar;

                if (c != mLanguageDefinition.mPreprocChar && !isspace(c))
                    firstChar = false;

                if (currentIndex == (int)line.size() - 1 && line[line.size() - 1].mChar == '\\')
                    concatenate = true;

                bool inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

                if (withinString) {
                    line[currentIndex].mMultiLineComment = inComment;

                    if (c == '\"') {
                        if (currentIndex + 1 < (int)line.size() && line[currentIndex + 1].mChar == '\"') {
                            currentIndex += 1;
                            if (currentIndex < (int)line.size())
                                line[currentIndex].mMultiLineComment = inComment;
                        } else
                            withinString = false;
                    } else if (c == '\\') {
                        currentIndex += 1;
                        if (currentIndex < (int)line.size())
                            line[currentIndex].mMultiLineComment = inComment;
                    }
                } else {
                    if (firstChar && c == mLanguageDefinition.mPreprocChar)
                        withinPreproc = true;

                    if (c == '\"') {
                        withinString = true;
                        line[currentIndex].mMultiLineComment = inComment;
                    } else {
                        auto pred = [](const char& a, const Glyph& b) { return a == b.mChar; };
                        auto from = line.begin() + currentIndex;
                        auto& startStr = mLanguageDefinition.mCommentStart;
                        auto& singleStartStr = mLanguageDefinition.mSingleLineComment;

                        if (singleStartStr.size() > 0 && currentIndex + singleStartStr.size() <= line.size() &&
                            equals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred)) {
                            withinSingleLineComment = true;
                        } else if (!withinSingleLineComment && currentIndex + startStr.size() <= line.size() &&
                                   equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred)) {
                            commentStartLine = currentLine;
                            commentStartIndex = currentIndex;
                        }

                        inComment = inComment =
                            (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

                        line[currentIndex].mMultiLineComment = inComment;
                        line[currentIndex].mComment = withinSingleLineComment;

                        auto& endStr = mLanguageDefinition.mCommentEnd;
                        if (currentIndex + 1 >= (int)endStr.size() &&
                            equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred)) {
                            commentStartIndex = endIndex;
                            commentStartLine = endLine;
                        }
                    }
                }
                line[currentIndex].mPreprocessor = withinPreproc;
                currentIndex += UTF8CharLength(c);
                if (currentIndex >= (int)line.size()) {
                    currentIndex = 0;
                    ++currentLine;
                }
            } else {
                currentIndex = 0;
                ++currentLine;
            }
        }
        mCheckComments = false;
    }

    if (mColorRangeMin < mColorRangeMax) {
        const int increment = (mLanguageDefinition.mTokenize == nullptr) ? 10 : 10000;
        const int to = min(mColorRangeMin + increment, mColorRangeMax);
        ColorizeRange(mColorRangeMin, to);
        mColorRangeMin = to;

        if (mColorRangeMax == mColorRangeMin) {
            mColorRangeMin = 10000;
            mColorRangeMax = 0;
        }
        return;
    }
}

float TextEditor::TextDistanceToLineStart(const Coordinates& aFrom) const {
    auto& line = mLines[aFrom.mLine];
    float distance = 0.0f;
    float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;
    int colIndex = GetCharacterIndex(aFrom);
    for (size_t it = 0u; it < line.size() && it < colIndex;) {
        if (line[it].mChar == '\t') {
            distance = (1.0f + std::floor((1.0f + distance) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
            ++it;
        } else {
            auto d = UTF8CharLength(line[it].mChar);
            char tempCString[7];
            int i = 0;
            for (; i < 6 && d-- > 0 && it < (int)line.size(); i++, it++)
                tempCString[i] = line[it].mChar;

            tempCString[i] = '\0';
            distance += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, tempCString, nullptr, nullptr).x;
        }
    }

    return distance;
}

void TextEditor::EnsureCursorVisible() {
    if (!mWithinRender) {
        mScrollToCursor = true;
        return;
    }

    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto height = ImGui::GetWindowHeight();
    auto width = ImGui::GetWindowWidth();

    auto top = 1 + (int)ceil(scrollY / mCharAdvance.y);
    auto bottom = (int)ceil((scrollY + height) / mCharAdvance.y);

    auto left = (int)ceil(scrollX / mCharAdvance.x);
    auto right = (int)ceil((scrollX + width) / mCharAdvance.x);

    auto pos = GetActualCursorCoordinates();
    auto len = TextDistanceToLineStart(pos);

    if (pos.mLine < top)
        ImGui::SetScrollY(max(0.0f, (pos.mLine - 1) * mCharAdvance.y));
    if (pos.mLine > bottom - 4)
        ImGui::SetScrollY(max(0.0f, (pos.mLine + 4) * mCharAdvance.y - height));
    if (len + mTextStart < left + 4)
        ImGui::SetScrollX(max(0.0f, len + mTextStart - 4));
    if (len + mTextStart > right - 4)
        ImGui::SetScrollX(max(0.0f, len + mTextStart + 4 - width));
}

int TextEditor::GetPageSize() const {
    auto height = ImGui::GetWindowHeight() - 20.0f;
    return (int)floor(height / mCharAdvance.y);
}

TextEditor::UndoRecord::UndoRecord(const std::string& aAdded, const TextEditor::Coordinates aAddedStart,
    const TextEditor::Coordinates aAddedEnd, const std::string& aRemoved, const TextEditor::Coordinates aRemovedStart,
    const TextEditor::Coordinates aRemovedEnd, TextEditor::EditorState& aBefore, TextEditor::EditorState& aAfter)
    : mAdded(aAdded)
    , mAddedStart(aAddedStart)
    , mAddedEnd(aAddedEnd)
    , mRemoved(aRemoved)
    , mRemovedStart(aRemovedStart)
    , mRemovedEnd(aRemovedEnd)
    , mBefore(aBefore)
    , mAfter(aAfter) {
    assert(mAddedStart <= mAddedEnd);
    assert(mRemovedStart <= mRemovedEnd);
}

void TextEditor::UndoRecord::Undo(TextEditor* aEditor) {
    if (!mAdded.empty()) {
        aEditor->DeleteRange(mAddedStart, mAddedEnd);
        aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 2);
    }

    if (!mRemoved.empty()) {
        auto start = mRemovedStart;
        aEditor->InsertTextAt(start, mRemoved.c_str());
        aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 2);
    }

    aEditor->mState = mBefore;
    aEditor->EnsureCursorVisible();
}

void TextEditor::UndoRecord::Redo(TextEditor* aEditor) {
    if (!mRemoved.empty()) {
        aEditor->DeleteRange(mRemovedStart, mRemovedEnd);
        aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 1);
    }

    if (!mAdded.empty()) {
        auto start = mAddedStart;
        aEditor->InsertTextAt(start, mAdded.c_str());
        aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 1);
    }

    aEditor->mState = mAfter;
    aEditor->EnsureCursorVisible();
}

static bool TokenizeCStyleString(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end) {
    const char* p = in_begin;

    if (*p == '"') {
        p++;

        while (p < in_end) {
            // handle end of string
            if (*p == '"') {
                out_begin = in_begin;
                out_end = p + 1;
                return true;
            }

            // handle escape character for "
            if (*p == '\\' && p + 1 < in_end && p[1] == '"')
                p++;

            p++;
        }
    }

    return false;
}

static bool TokenizeCStyleCharacterLiteral(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end) {
    const char* p = in_begin;

    if (*p == '\'') {
        p++;

        // handle escape characters
        if (p < in_end && *p == '\\')
            p++;

        if (p < in_end)
            p++;

        // handle end of character literal
        if (p < in_end && *p == '\'') {
            out_begin = in_begin;
            out_end = p + 1;
            return true;
        }
    }

    return false;
}

static bool TokenizeCStyleIdentifier(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end) {
    const char* p = in_begin;

    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        p++;

        while ((p < in_end) && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            p++;

        out_begin = in_begin;
        out_end = p;
        return true;
    }

    return false;
}

static bool TokenizeCStyleNumber(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end) {
    const char* p = in_begin;

    const bool startsWithNumber = *p >= '0' && *p <= '9';

    if (*p != '+' && *p != '-' && !startsWithNumber)
        return false;

    p++;

    bool hasNumber = startsWithNumber;

    while (p < in_end && (*p >= '0' && *p <= '9')) {
        hasNumber = true;

        p++;
    }

    if (hasNumber == false)
        return false;

    bool isFloat = false;
    bool isHex = false;
    bool isBinary = false;

    if (p < in_end) {
        if (*p == '.') {
            isFloat = true;

            p++;

            while (p < in_end && (*p >= '0' && *p <= '9'))
                p++;
        } else if (*p == 'x' || *p == 'X') {
            // hex formatted integer of the type 0xef80

            isHex = true;

            p++;

            while (p < in_end && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
                p++;
        } else if (*p == 'b' || *p == 'B') {
            // binary formatted integer of the type 0b01011101

            isBinary = true;

            p++;

            while (p < in_end && (*p >= '0' && *p <= '1'))
                p++;
        }
    }

    if (isHex == false && isBinary == false) {
        // floating point exponent
        if (p < in_end && (*p == 'e' || *p == 'E')) {
            isFloat = true;

            p++;

            if (p < in_end && (*p == '+' || *p == '-'))
                p++;

            bool hasDigits = false;

            while (p < in_end && (*p >= '0' && *p <= '9')) {
                hasDigits = true;

                p++;
            }

            if (hasDigits == false)
                return false;
        }

        // single precision floating point type
        if (p < in_end && *p == 'f')
            p++;
    }

    if (isFloat == false) {
        // integer size type
        while (p < in_end && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L'))
            p++;
    }

    out_begin = in_begin;
    out_end = p;
    return true;
}

static bool TokenizeCStylePunctuation(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end) {
    (void)in_end;

    switch (*in_begin) {
    case '[':
    case ']':
    case '{':
    case '}':
    case '!':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '-':
    case '+':
    case '=':
    case '~':
    case '|':
    case '<':
    case '>':
    case '?':
    case ':':
    case '/':
    case ';':
    case ',':
    case '.':
        out_begin = in_begin;
        out_end = in_begin + 1;
        return true;
    }

    return false;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::CPlusPlus() {
    static bool inited = false;
    static LanguageDefinition langDef;
    if (!inited) {
        static const char* const cppKeywords[] = {"alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
            "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char16_t", "char32_t", "class",
            "compl", "concept", "const", "constexpr", "const_cast", "continue", "decltype", "default", "delete", "do", "double",
            "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "import",
            "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
            "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
            "static", "static_assert", "static_cast", "struct", "switch", "synchronized", "template", "this", "thread_local", "throw",
            "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
            "xor", "xor_eq"};
        for (auto& k : cppKeywords)
            langDef.mKeywords.insert(k);

        static const char* const identifiers[] = {"abort", "abs", "acos", "asin", "atan", "atexit", "atof", "atoi", "atol", "ceil", "clock",
            "cosh", "ctime", "div", "exit", "fabs", "floor", "fmod", "getchar", "getenv", "isalnum", "isalpha", "isdigit", "isgraph",
            "ispunct", "isspace", "isupper", "kbhit", "log10", "log2", "log", "memcmp", "modf", "pow", "printf", "sprintf", "snprintf",
            "putchar", "putenv", "puts", "rand", "remove", "rename", "sinh", "sqrt", "srand", "strcat", "strcmp", "strerror", "time",
            "tolower", "toupper", "std", "string", "vector", "map", "unordered_map", "set", "unordered_set", "min", "max"};
        for (auto& k : identifiers) {
            Identifier id;
            id.mDeclaration = "Built-in function";
            langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
        }

        langDef.mTokenize = [](const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end,
                                PaletteIndex& paletteIndex) -> bool {
            paletteIndex = PaletteIndex::Max;

            while (in_begin < in_end && isascii(*in_begin) && isblank(*in_begin))
                in_begin++;

            if (in_begin == in_end) {
                out_begin = in_end;
                out_end = in_end;
                paletteIndex = PaletteIndex::Default;
            } else if (TokenizeCStyleString(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::String;
            else if (TokenizeCStyleCharacterLiteral(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::CharLiteral;
            else if (TokenizeCStyleIdentifier(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Identifier;
            else if (TokenizeCStyleNumber(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Number;
            else if (TokenizeCStylePunctuation(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Punctuation;

            return paletteIndex != PaletteIndex::Max;
        };

        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";
        langDef.mSingleLineComment = "//";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;

        langDef.mName = "C++";

        inited = true;
    }
    return langDef;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::HLSL() {
    static bool inited = false;
    static LanguageDefinition langDef;
    if (!inited) {
        static const char* const keywords[] = {
            "AppendStructuredBuffer",
            "asm",
            "asm_fragment",
            "BlendState",
            "bool",
            "break",
            "Buffer",
            "ByteAddressBuffer",
            "case",
            "cbuffer",
            "centroid",
            "class",
            "column_major",
            "compile",
            "compile_fragment",
            "CompileShader",
            "const",
            "continue",
            "ComputeShader",
            "ConsumeStructuredBuffer",
            "default",
            "DepthStencilState",
            "DepthStencilView",
            "discard",
            "do",
            "double",
            "DomainShader",
            "dword",
            "else",
            "export",
            "extern",
            "false",
            "float",
            "for",
            "fxgroup",
            "GeometryShader",
            "groupshared",
            "half",
            "Hullshader",
            "if",
            "in",
            "inline",
            "inout",
            "InputPatch",
            "int",
            "interface",
            "line",
            "lineadj",
            "linear",
            "LineStream",
            "matrix",
            "min16float",
            "min10float",
            "min16int",
            "min12int",
            "min16uint",
            "namespace",
            "nointerpolation",
            "noperspective",
            "NULL",
            "out",
            "OutputPatch",
            "packoffset",
            "pass",
            "pixelfragment",
            "PixelShader",
            "point",
            "PointStream",
            "precise",
            "RasterizerState",
            "RenderTargetView",
            "return",
            "register",
            "row_major",
            "RWBuffer",
            "RWByteAddressBuffer",
            "RWStructuredBuffer",
            "RWTexture1D",
            "RWTexture1DArray",
            "RWTexture2D",
            "RWTexture2DArray",
            "RWTexture3D",
            "sample",
            "sampler",
            "SamplerState",
            "SamplerComparisonState",
            "shared",
            "snorm",
            "stateblock",
            "stateblock_state",
            "static",
            "string",
            "struct",
            "switch",
            "StructuredBuffer",
            "tbuffer",
            "technique",
            "technique10",
            "technique11",
            "texture",
            "Texture1D",
            "Texture1DArray",
            "Texture2D",
            "Texture2DArray",
            "Texture2DMS",
            "Texture2DMSArray",
            "Texture3D",
            "TextureCube",
            "TextureCubeArray",
            "true",
            "typedef",
            "triangle",
            "triangleadj",
            "TriangleStream",
            "uint",
            "uniform",
            "unorm",
            "unsigned",
            "vector",
            "vertexfragment",
            "VertexShader",
            "void",
            "volatile",
            "while",
            "bool1",
            "bool2",
            "bool3",
            "bool4",
            "double1",
            "double2",
            "double3",
            "double4",
            "float1",
            "float2",
            "float3",
            "float4",
            "int1",
            "int2",
            "int3",
            "int4",
            "in",
            "out",
            "inout",
            "uint1",
            "uint2",
            "uint3",
            "uint4",
            "dword1",
            "dword2",
            "dword3",
            "dword4",
            "half1",
            "half2",
            "half3",
            "half4",
            "float1x1",
            "float2x1",
            "float3x1",
            "float4x1",
            "float1x2",
            "float2x2",
            "float3x2",
            "float4x2",
            "float1x3",
            "float2x3",
            "float3x3",
            "float4x3",
            "float1x4",
            "float2x4",
            "float3x4",
            "float4x4",
            "half1x1",
            "half2x1",
            "half3x1",
            "half4x1",
            "half1x2",
            "half2x2",
            "half3x2",
            "half4x2",
            "half1x3",
            "half2x3",
            "half3x3",
            "half4x3",
            "half1x4",
            "half2x4",
            "half3x4",
            "half4x4",
        };
        for (auto& k : keywords)
            langDef.mKeywords.insert(k);

        static const char* const identifiers[] = {"abort", "abs", "acos", "all", "AllMemoryBarrier", "AllMemoryBarrierWithGroupSync", "any",
            "asdouble", "asfloat", "asin", "asint", "asint", "asuint", "asuint", "atan", "atan2", "ceil", "CheckAccessFullyMapped", "clamp",
            "clip", "cos", "cosh", "countbits", "cross", "D3DCOLORtoUBYTE4", "ddx", "ddx_coarse", "ddx_fine", "ddy", "ddy_coarse",
            "ddy_fine", "degrees", "determinant", "DeviceMemoryBarrier", "DeviceMemoryBarrierWithGroupSync", "distance", "dot", "dst",
            "errorf", "EvaluateAttributeAtCentroid", "EvaluateAttributeAtSample", "EvaluateAttributeSnapped", "exp", "exp2", "f16tof32",
            "f32tof16", "faceforward", "firstbithigh", "firstbitlow", "floor", "fma", "fmod", "frac", "frexp", "fwidth",
            "GetRenderTargetSampleCount", "GetRenderTargetSamplePosition", "GroupMemoryBarrier", "GroupMemoryBarrierWithGroupSync",
            "InterlockedAdd", "InterlockedAnd", "InterlockedCompareExchange", "InterlockedCompareStore", "InterlockedExchange",
            "InterlockedMax", "InterlockedMin", "InterlockedOr", "InterlockedXor", "isfinite", "isinf", "isnan", "ldexp", "length", "lerp",
            "lit", "log", "log10", "log2", "mad", "max", "min", "modf", "msad4", "mul", "noise", "normalize", "pow", "printf",
            "Process2DQuadTessFactorsAvg", "Process2DQuadTessFactorsMax", "Process2DQuadTessFactorsMin", "ProcessIsolineTessFactors",
            "ProcessQuadTessFactorsAvg", "ProcessQuadTessFactorsMax", "ProcessQuadTessFactorsMin", "ProcessTriTessFactorsAvg",
            "ProcessTriTessFactorsMax", "ProcessTriTessFactorsMin", "radians", "rcp", "reflect", "refract", "reversebits", "round", "rsqrt",
            "saturate", "sign", "sin", "sincos", "sinh", "smoothstep", "sqrt", "step", "tan", "tanh", "tex1D", "tex1D", "tex1Dbias",
            "tex1Dgrad", "tex1Dlod", "tex1Dproj", "tex2D", "tex2D", "tex2Dbias", "tex2Dgrad", "tex2Dlod", "tex2Dproj", "tex3D", "tex3D",
            "tex3Dbias", "tex3Dgrad", "tex3Dlod", "tex3Dproj", "texCUBE", "texCUBE", "texCUBEbias", "texCUBEgrad", "texCUBElod",
            "texCUBEproj", "transpose", "trunc"};
        for (auto& k : identifiers) {
            Identifier id;
            id.mDeclaration = "Built-in function";
            langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
        }

        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("[ \\t]*#[ \\t]*[a-zA-Z_]+", PaletteIndex::Preprocessor));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("\\'\\\\?[^\\']\\'", PaletteIndex::CharLiteral));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", PaletteIndex::Identifier));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>(
            "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PaletteIndex::Punctuation));

        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";
        langDef.mSingleLineComment = "//";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;

        langDef.mName = "HLSL";

        inited = true;
    }
    return langDef;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::GLSL() {
    static bool inited = false;
    static LanguageDefinition langDef;
    if (!inited) {
        static const char* const keywords[] = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
            "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
            "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
            "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"};
        for (auto& k : keywords)
            langDef.mKeywords.insert(k);

        static const char* const identifiers[] = {"abort", "abs", "acos", "asin", "atan", "atexit", "atof", "atoi", "atol", "ceil", "clock",
            "cosh", "ctime", "div", "exit", "fabs", "floor", "fmod", "getchar", "getenv", "isalnum", "isalpha", "isdigit", "isgraph",
            "ispunct", "isspace", "isupper", "kbhit", "log10", "log2", "log", "memcmp", "modf", "pow", "putchar", "putenv", "puts", "rand",
            "remove", "rename", "sinh", "sqrt", "srand", "strcat", "strcmp", "strerror", "time", "tolower", "toupper"};
        for (auto& k : identifiers) {
            Identifier id;
            id.mDeclaration = "Built-in function";
            langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
        }

        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("[ \\t]*#[ \\t]*[a-zA-Z_]+", PaletteIndex::Preprocessor));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("\\'\\\\?[^\\']\\'", PaletteIndex::CharLiteral));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", PaletteIndex::Identifier));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>(
            "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PaletteIndex::Punctuation));

        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";
        langDef.mSingleLineComment = "//";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;

        langDef.mName = "GLSL";

        inited = true;
    }
    return langDef;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::C() {
    static bool inited = false;
    static LanguageDefinition langDef;
    if (!inited) {
        static const char* const keywords[] = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
            "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
            "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
            "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"};
        for (auto& k : keywords)
            langDef.mKeywords.insert(k);

        static const char* const identifiers[] = {"abort", "abs", "acos", "asin", "atan", "atexit", "atof", "atoi", "atol", "ceil", "clock",
            "cosh", "ctime", "div", "exit", "fabs", "floor", "fmod", "getchar", "getenv", "isalnum", "isalpha", "isdigit", "isgraph",
            "ispunct", "isspace", "isupper", "kbhit", "log10", "log2", "log", "memcmp", "modf", "pow", "putchar", "putenv", "puts", "rand",
            "remove", "rename", "sinh", "sqrt", "srand", "strcat", "strcmp", "strerror", "time", "tolower", "toupper"};
        for (auto& k : identifiers) {
            Identifier id;
            id.mDeclaration = "Built-in function";
            langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
        }

        langDef.mTokenize = [](const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end,
                                PaletteIndex& paletteIndex) -> bool {
            paletteIndex = PaletteIndex::Max;

            while (in_begin < in_end && isascii(*in_begin) && isblank(*in_begin))
                in_begin++;

            if (in_begin == in_end) {
                out_begin = in_end;
                out_end = in_end;
                paletteIndex = PaletteIndex::Default;
            } else if (TokenizeCStyleString(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::String;
            else if (TokenizeCStyleCharacterLiteral(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::CharLiteral;
            else if (TokenizeCStyleIdentifier(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Identifier;
            else if (TokenizeCStyleNumber(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Number;
            else if (TokenizeCStylePunctuation(in_begin, in_end, out_begin, out_end))
                paletteIndex = PaletteIndex::Punctuation;

            return paletteIndex != PaletteIndex::Max;
        };

        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";
        langDef.mSingleLineComment = "//";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;

        langDef.mName = "C";

        inited = true;
    }
    return langDef;
}


const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::Lua() {
    static bool inited = false;
    static LanguageDefinition langDef;
    if (!inited) {
        static const char* const keywords[] = {"and", "break", "do", "", "else", "elseif", "end", "false", "for", "function", "if", "in",
            "", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};

        for (auto& k : keywords)
            langDef.mKeywords.insert(k);

        static const char* const identifiers[] = {"assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs",
            "load", "next", "pairs", "pcall", "print", "rawequal", "rawlen", "rawget", "rawset", "select", "setmetatable",
            "tonumber", "tostring", "type", "xpcall", "_G", "_VERSION", "_ENV", "and", "not", "or",  
            "create", "resume", "running", "status", "wrap", "yield", "isyieldable",
            "lines", "open", "output", "read", "tmpfile", "type", "write", "close", "flush", "lines", "read", "seek", "setvbuf",
            "write", "__gc", "__tostring", "abs", "acos", "asin", "atan", "ceil", "cos", "deg", "exp", "tointeger", "floor", "fmod", "ult",
            "log", "max", "min", "modf", "rad", "random", "randomseed", "sin", "sqrt", "string", "tan", "type", "cosh", "sinh",
            "tanh", "pow", "frexp", "ldexp", "log10", "pi", "huge", "maxinteger", "mininteger", "loadlib", "searchpath", "seeall",
            "preload", "cpath", "path", "searchers", "loaded", "module", "require", "clock", "date", "difftime", "execute", "exit",
            "getenv", "remove", "rename", "setlocale", "time", "tmpname", "byte", "char", "dump", "find", "format", "gmatch", "gsub", "len",
            "lower", "match", "rep", "reverse", "sub", "upper", "pack", "packsize", "unpack", "concat", "maxn", "insert", "pack", "unpack",
            "remove", "move", "sort", "offset", "codepoint", "char", "len", "codes", "charpattern", "coroutine", "table", "io", "os", "uevr", "api",
            " UEVR_UObjectHook", "UEVR_UObject", "UEVR_UClass", "UEVR_UFunction", "as_struct", "as_class", "as_function", "get_class",
            "super", "to_string", "get_fname", "find_uobject", "to_uobject", "get_player_controller", "add_component_by_class",
            "spawn_object", "get_local_pawn", "get_address", "uevr.sdk.callbacks", "is_runtime_ready", "is_hmd_active", "get_uengine",
            "Vector3f", "Vector4f", "Vector2f", "Quaternionf", "Quaterniond",
            "get_objects_matching",
            "string", "utf8", "bit32", "math", "package"};
        for (auto& k : identifiers) {
            Identifier id;
            id.mDeclaration = "Built-in function";
            langDef.mIdentifiers.insert(std::make_pair(std::string(k), id));
        }

        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("\\\'[^\\\']*\\\'", PaletteIndex::String));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(
            std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", PaletteIndex::Identifier));
        langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>(
            "[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PaletteIndex::Punctuation));

        langDef.mCommentStart = "--[[";
        langDef.mCommentEnd = "]]";
        langDef.mSingleLineComment = "--";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = false;

        langDef.mName = "Lua";

        inited = true;
    }
    return langDef;
}

using namespace uevr;

#define PLUGIN_LOG_ONCE(...) \
    static bool _logged_ = false; \
    if (!_logged_) { \
        _logged_ = true; \
        API::get()->log_info(__VA_ARGS__); \
    }
static std::string lua_text{};
static inline TextEditor text_editor;
class ExamplePlugin : public uevr::Plugin {
public:
    ExamplePlugin() = default;

    void on_dllmain() override {}

    void on_initialize() override {
        ImGui::CreateContext();

        API::get()->log_error("%s %s", "Hello", "error");
        API::get()->log_warn("%s %s", "Hello", "warning");
        API::get()->log_info("%s %s", "Hello", "info");
    }

    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        if (!m_initialized) {
            if (!initialize_imgui()) {
                API::get()->log_info("Failed to initialize imgui");
                return;
            } else {
                API::get()->log_info("Initialized imgui");
            }
        }

        const auto renderer_data = API::get()->param()->renderer;

        if (!API::get()->param()->vr->is_hmd_active()) {
            if (!m_was_rendering_desktop) {
                m_was_rendering_desktop = true;
                on_device_reset();
                return;
            }

            m_was_rendering_desktop = true;

            if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
                ImGui_ImplDX11_NewFrame();
                g_d3d11.render_imgui();
            } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
                auto command_queue = (ID3D12CommandQueue*)renderer_data->command_queue;

                if (command_queue == nullptr) {
                    return;
                }

                ImGui_ImplDX12_NewFrame();
                g_d3d12.render_imgui();
            }
        }
    }

    void reset_height() {
        auto& api = API::get();
        auto vr = api->param()->vr;
        UEVR_Vector3f origin{};
        vr->get_standing_origin(&origin);

        UEVR_Vector3f hmd_pos{};
        UEVR_Quaternionf hmd_rot{};
        vr->get_pose(vr->get_hmd_index(), &hmd_pos, &hmd_rot);

        origin.y = hmd_pos.y;

        vr->set_standing_origin(&origin);
    }

    void on_device_reset() override {
        PLUGIN_LOG_ONCE("Example Device Reset");

        std::scoped_lock _{m_imgui_mutex};

        const auto renderer_data = API::get()->param()->renderer;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        m_initialized = false;
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {
        PLUGIN_LOG_ONCE("Post Render VR Framework DX11");

        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (!m_initialized || !vr_active) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        PLUGIN_LOG_ONCE("Post Render VR Framework DX12");

        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (!m_initialized || !vr_active) {
            return;
        }

        if (m_was_rendering_desktop) {
            m_was_rendering_desktop = false;
            on_device_reset();
            return;
        }

        std::scoped_lock _{m_imgui_mutex};

        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(command_list, rtv);
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override { 
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

    void on_custom_event(const char* event_name, const char* event_data) override {
        API::get()->log_info("Custom Event: %s %s", event_name, event_data);
    }

    void print_all_objects() {
        API::get()->log_info("Printing all objects...");

        API::get()->log_info("Chunked: %i", API::FUObjectArray::is_chunked());
        API::get()->log_info("Inlined: %i", API::FUObjectArray::is_inlined());
        API::get()->log_info("Objects offset: %i", API::FUObjectArray::get_objects_offset());
        API::get()->log_info("Item distance: %i", API::FUObjectArray::get_item_distance());
        API::get()->log_info("Object count: %i", API::FUObjectArray::get()->get_object_count());

        const auto objects = API::FUObjectArray::get();

        if (objects == nullptr) {
            API::get()->log_error("Failed to get FUObjectArray");
            return;
        }

        for (int32_t i = 0; i < objects->get_object_count(); ++i) {
            const auto object = objects->get_object(i);

            if (object == nullptr) {
                continue;
            }

            const auto name = object->get_full_name();

            if (name.empty()) {
                continue;
            }

            std::string name_narrow{std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(name)};

            API::get()->log_info(" [%d]: %s", i, name_narrow.c_str());
        }
    }

    // Test attaching skeletal mesh components with UObjectHook.
    void test_mesh_attachment() {
        struct {
            API::UClass* c;
            API::TArray<API::UObject*> return_value{};
        } component_params;

        component_params.c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
        const auto pawn = API::get()->get_local_pawn(0);

        if (component_params.c != nullptr && pawn != nullptr) {
            // either or.
            pawn->call_function(L"K2_GetComponentsByClass", &component_params);
            pawn->call_function(L"GetComponentsByClass", &component_params);

            if (component_params.return_value.empty()) {
                API::get()->log_error("Failed to find any SkeletalMeshComponents");
            }

            for (auto mesh : component_params.return_value) {
                auto state = API::UObjectHook::get_or_add_motion_controller_state(mesh);
            }
        } else {
            API::get()->log_error("Failed to find SkeletalMeshComponent class or local pawn");
        }
    }

    void test_console_manager() {
        const auto console_manager = API::get()->get_console_manager();

        if (console_manager != nullptr) {
            API::get()->log_info("Console manager @ 0x%p", console_manager);
            const auto& objects = console_manager->get_console_objects();

            for (const auto& object : objects) {
                if (object.key != nullptr) {
                    // convert from wide to narrow string (we do not have utility::narrow in this context).
                    std::string key_narrow{std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(object.key)};
                    if (object.value != nullptr) {
                        const auto command = object.value->as_command();

                        if (command != nullptr) {
                            API::get()->log_info(" Console COMMAND: %s @ 0x%p", key_narrow.c_str(), object.value);
                        } else {
                            API::get()->log_info(" Console VARIABLE: %s @ 0x%p", key_narrow.c_str(), object.value);
                        }
                    }
                }
            }

            auto cvar = console_manager->find_variable(L"r.Color.Min");

            if (cvar != nullptr) {
                API::get()->log_info("Found r.Color.Min @ 0x%p (%f)", cvar, cvar->get_float());
            } else {
                API::get()->log_error("Failed to find r.Color.Min");
            }

            auto cvar2 = console_manager->find_variable(L"r.Upscale.Quality");

            if (cvar2 != nullptr) {
                API::get()->log_info("Found r.Upscale.Quality @ 0x%p (%d)", cvar2, cvar2->get_int());
                cvar2->set(cvar2->get_int() + 1);
            } else {
                API::get()->log_error("Failed to find r.Upscale.Quality");
            }
        } else {
            API::get()->log_error("Failed to find console manager");
        }
    }
    


    void test_engine(API::UGameEngine* engine) {
        // Log the UEngine name.
        const auto uengine_name = engine->get_full_name();

        // Convert from wide to narrow string (we do not have utility::narrow in this context).
        std::string uengine_name_narrow{std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(uengine_name)};

        API::get()->log_info("Engine name: %s", uengine_name_narrow.c_str());

        // Test if we can dcast to UObject.
        {
            const auto engine_as_object = engine->dcast<API::UObject>();

            if (engine != nullptr) {
                API::get()->log_info("Engine successfully dcast to UObject");
            } else {
                API::get()->log_error("Failed to dcast Engine to UObject");
            }
        }

        // Go through all of engine's fields and log their names.
        const auto engine_class_ours = (API::UStruct*)engine->get_class();
        for (auto super = engine_class_ours; super != nullptr; super = super->get_super()) {
            for (auto field = super->get_child_properties(); field != nullptr; field = field->get_next()) {
                const auto field_fname = field->get_fname();
                const auto field_name = field_fname->to_string();
                const auto field_class = field->get_class();

                std::wstring prepend{};

                if (field_class != nullptr) {
                    const auto field_class_fname = field_class->get_fname();
                    const auto field_class_name = field_class_fname->to_string();

                    prepend = field_class_name + L" ";
                }

                // Convert from wide to narrow string (we do not have utility::narrow in this context).
                std::string field_name_narrow{std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(prepend + field_name)};
                API::get()->log_info(" Field name: %s", field_name_narrow.c_str());
            }
        }

        // Check if we can find the GameInstance and call is_a() on it.
        const auto game_instance = engine->get_property<API::UObject*>(L"GameInstance");

        if (game_instance != nullptr) {
            const auto game_instance_class = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameInstance");

            if (game_instance->is_a(game_instance_class)) {
                const auto& local_players = game_instance->get_property<API::TArray<API::UObject*>>(L"LocalPlayers");

                if (local_players.count > 0 && local_players.data != nullptr) {
                    const auto local_player = local_players.data[0];

                    
                } else {
                    API::get()->log_error("Failed to find LocalPlayers");
                }

                API::get()->log_info("GameInstance is a UGameInstance");
            } else {
                API::get()->log_error("GameInstance is not a UGameInstance");
            }
        } else {
            API::get()->log_error("Failed to find GameInstance");
        }

        // Find the Engine object and compare it to the one we have.
        const auto engine_class = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameEngine");
        if (engine_class != nullptr) {
            // Round 1, check if we can find it via get_first_object_by_class.
            const auto engine_searched = engine_class->get_first_object_matching<API::UGameEngine>(false);

            if (engine_searched != nullptr) {
                if (engine_searched == engine) {
                    API::get()->log_info("Found Engine object @ 0x%p", engine_searched);
                } else {
                    API::get()->log_error("Found Engine object @ 0x%p, but it's not the same as the one we have", engine_searched);
                }
            } else {
                API::get()->log_error("Failed to find Engine object");
            }

            // Round 2, check if we can find it via get_objects_by_class.
            const auto objects = engine_class->get_objects_matching<API::UGameEngine>(false);

            if (!objects.empty()) {
                for (const auto& obj : objects) {
                    if (obj == engine) {
                        API::get()->log_info("Found Engine object @ 0x%p", obj);
                    } else {
                        API::get()->log_info("Found unrelated Engine object @ 0x%p", obj);
                    }
                }
            } else {
                API::get()->log_error("Failed to find Engine objects");
            }
        } else {
            API::get()->log_error("Failed to find Engine class");
        }
    }

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        PLUGIN_LOG_ONCE("Pre Engine Tick: %f", delta);

        static bool once = true;
        static bool openwindow = true;
        // Unit tests for the API basically.
        if (once) {
            once = false;

            API::get()->log_info("Running once on pre engine tick");
            API::get()->execute_command(L"stat fps");

            API::FName test_name{L"Left"};
            std::string name_narrow{std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(test_name.to_string())};
            API::get()->log_info("Test FName: %s", name_narrow.c_str());

            print_all_objects();
            test_mesh_attachment();
            test_console_manager();
            test_engine(engine);
        }

        if (m_initialized) {
            std::scoped_lock _{m_imgui_mutex};

            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (openwindow) {
                internal_frame();
            }
            if( ImGui::IsKeyReleased(ImGuiKey_F2))
                openwindow = !openwindow;

            ImGui::EndFrame();
            ImGui::Render();
        }
    }

    void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
        PLUGIN_LOG_ONCE("Post Engine Tick: %f", delta);
    }

    void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        PLUGIN_LOG_ONCE("Pre Slate Draw Window");
    }

    void on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
        PLUGIN_LOG_ONCE("Post Slate Draw Window");
    }

    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                             UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override
    {
        PLUGIN_LOG_ONCE("Pre Calculate Stereo View Offset");

        auto rotationd = (UEVR_Rotatord*)rotation;

        // Decoupled pitch.
        if (!is_double) {
            rotation->pitch = 0.0f;
        } else {
            rotationd->pitch = 0.0;
        }
    }

    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float world_to_meters, 
                                              UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double)
    {
        PLUGIN_LOG_ONCE("Post Calculate Stereo View Offset");
    }

    void on_pre_viewport_client_draw(UEVR_UGameViewportClientHandle viewport_client, UEVR_FViewportHandle viewport, UEVR_FCanvasHandle canvas) {
        PLUGIN_LOG_ONCE("Pre Viewport Client Draw");
    }

    void on_post_viewport_client_draw(UEVR_UGameViewportClientHandle viewport_client, UEVR_FViewportHandle viewport, UEVR_FCanvasHandle canvas) {
        PLUGIN_LOG_ONCE("Post Viewport Client Draw");
    }

private:
    bool initialize_imgui() {
        if (m_initialized) {
            return true;
        }

        std::scoped_lock _{m_imgui_mutex};

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        static const auto imgui_ini = API::get()->get_persistent_dir(L"imgui_example_plugin.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        const auto renderer_data = API::get()->param()->renderer;

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
        swapchain->GetDesc(&swap_desc);

        m_wnd = swap_desc.OutputWindow;

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        }

        m_initialized = true;
        return true;
    }

    std::string_view read_file(std::filesystem::path path ){
            std::ifstream inputFile(path.string());
            std::stringstream buffer;
            buffer << inputFile.rdbuf();             // Read the entire file buffer into the stringstream
            std::string fileContents = buffer.str(); // Convert the stringstream to a std::string
            return fileContents;
    }

void spawn_mesh_widget()
{ 
    API::get()->dispatch_lua_event("exec", R"(
            local function func()
                local api = uevr.api
                local pawn = api:get_local_pawn(0)
              
                
                local widget = api:add_component_by_class(pawn, api:find_uobject("Class /Script/UMG/WidgetComponent"), false);
                widget.RelativeLocation = Vector3f.new(0, 0, 82) + (pawn:K2_GetActorForwardVector() * 64)
                

                    )");


}

    void internal_frame() {    
        
   
            ImGui::Begin("Lua Exec");  
            auto size = ImGui::GetContentRegionAvail();
            static bool open{false};
            ImGui::BeginChild("Console", ImVec2(size.x, size.y * 0.8f), true, ImGuiWindowFlags_AlwaysAutoResize);           
            if (ImGui::Button("Toggle Full Editor")) {
                full_editor = !full_editor; 
                if (full_editor) {
                                    text_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
                                    text_editor.SetPalette(TextEditor::GetDarkPalette());
                                    text_editor.SetTabSize(2);
                                    text_editor.SetShowWhitespaces(false);
                                    text_editor.SetColorizerEnable(true);
                                    text_editor.SetText(lua_text);
                    
                    }        
                }
                size = ImGui::GetContentRegionAvail();
                if (open) {
                       size.y *= 0.25f;
                }
            if (full_editor) {
                  
                    text_editor.Render("Lua Editor");
                    if (text_editor.IsTextChanged()) {
                        lua_text = text_editor.GetText();
                    }
            }
            else {
                static char input[4096]{};
                auto space = ImGui::GetContentRegionAvail();
                auto width = min(space.x * 0.75f, 250);
                auto linect = 3;
                for (auto c : input) {
                    if (c == '\n') {
                        ++linect;
                    }
                }
                auto height = linect * ImGui::CalcTextSize("T").y;
                if (ImGui::InputTextMultiline("##luainput", input, sizeof(input), ImGui::GetContentRegionAvail(),
                        ImGuiInputTextFlags_AllowTabInput |
                            ImGuiInputTextFlags_CallbackHistory)) {
                    lua_text = input;
                }

            }

            ImGui::EndChild();
            if (ImGui::Button("Execute")) {
                 API::get()->dispatch_lua_event("exec", lua_text);
            }
            ImGui::SameLine();
            static auto filepath = API::get()->get_persistent_dir() / "data"; 
            static char input[256]{};
            if (ImGui::InputText("Name", input, sizeof(input))) {
                  filepath /= input; 
            }
            if (ImGui::Button("Save") && !filepath.string().empty()) {
                std::filesystem::create_directories(filepath.parent_path());

                std::ofstream file{filepath};

                file << lua_text;                                                                         
            }
            ImGui::SameLine();  

            if (ImGui::Button("Browse for Script")) {
                open = ! open;
            }
            if (open) 
            {
                auto get_sorted_entries = [](const std::string& path, bool dirs_first) {
                    std::vector<std::pair<std::string, bool>> entries;
                    for (const auto& entry : std::filesystem::directory_iterator(path)) {
                        std::string name = entry.path().filename().string();
                        if (entry.path().has_extension() && entry.path().extension() != ".lua")
                            continue;
                        entries.emplace_back(name, entry.is_directory());
                    }
                    std::sort(entries.begin(), entries.end(), [dirs_first](const auto& a, const auto& b) {
                        if (a.second != b.second)
                            return dirs_first ? a.second > b.second : a.second < b.second;
                        return a.first < b.first;
                    });
                    return entries;
                };

                const auto activated_key = [](bool is_selected = false) -> bool {
                    return ImGui::IsMouseDoubleClicked(0) ||
                           is_selected && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                                              ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown));
                };
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0,0));
                ImGui::BeginPopupModal("File Browser", &open, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                if (ImGui::Button("Close")) 
                {
                
                        open = false;
                    ImGui::CloseCurrentPopup();                
                }
                static const std::filesystem::path scripts_path = API::get()->get_persistent_dir(L"scripts");
                static const std::filesystem::path global_path = API::get()->get_persistent_dir(L"..\\UEVR\\scripts");
                static const std::filesystem::path unrealvrmod = API::get()->get_persistent_dir(L"..");
   
                    static const std::filesystem::path downloads = std::filesystem::path(getenv("USERPROFILE")) / "Downloads";
                    static std::string current_path = scripts_path.string();
                    static std::string filter = "";
                    static char filter_buffer[256] = "";
                    bool dirs_first = true;
                    static bool only_lua = true;
                    static int selected_entry = -1;
                    static std::string script_path{};
                    ImGui::BeginChild("###filebrowser", ImVec2(700, 400), true,
                                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
                            // Set focus when opened
                            if (ImGui::IsWindowAppearing()) {
                                ImGui::SetWindowFocus();
                                selected_entry = -1;
                            }

                            ImGui::Text("Current Path: %s", current_path.c_str());

                            // Filter input
                            ImGui::InputText("Filter", filter_buffer, sizeof(filter_buffer), ImGuiInputTextFlags_EscapeClearsAll);
                            filter = filter_buffer;
                            static std::string copy_buffer{};
                            // Navigation buttons
                            bool can_go_up =
                                std::filesystem::path(current_path).has_parent_path() &&
                                std::filesystem::path(current_path).parent_path().string().find("UnrealVRMod") != std::string::npos;
                            if ((can_go_up && (ImGui::Button("Up") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) ||
                                                  ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) ||
                                ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                                current_path = std::filesystem::path(current_path).parent_path().string();
                                selected_entry = -1;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Home") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
                                current_path = scripts_path.string();
                                selected_entry = -1;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Global") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
                                current_path = global_path.string();
                                selected_entry = -1;
                            }

              

                            ImGui::BeginChild("FileList", ImVec2(0, 0), true);
                            try {
                                // Allow navigating to absolute path if entered in filter (for testing)
                                if (std::filesystem::path(filter).is_absolute() && std::filesystem::path(filter).has_stem() &&
                                    std::filesystem::exists(std::filesystem::path(filter))) {
                                    current_path = std::filesystem::path(filter).string();
                                    filter.clear();
                                    strncpy_s(filter_buffer, filter.c_str(), sizeof(filter_buffer));
                                    selected_entry = -1;
                                }

                                // Toggle directories first/last

                                if (ImGui::ArrowButton("##dirs_first", dirs_first ? ImGuiDir_Down : ImGuiDir_Up)) {
                                    dirs_first = !dirs_first;
                                }
                                ImGui::SameLine();
                                ImGui::Text(dirs_first ? "Sort Files First" : "Sort Directories First");
                                ImGui::Separator();

                                // Collect entries
                                std::vector<std::pair<std::string, bool>> entries; // {name, is_directory}
                                for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
                                    std::string name = entry.path().filename().string();
                                    if (only_lua && entry.is_regular_file() && entry.path().extension() != ".lua") {
                                        continue;
                                    }
                                    if (filter.empty() || name.find(filter) != std::string::npos) {
                                        entries.emplace_back(name, entry.is_directory());
                                    }
                                }

                                // Sort entries
                                std::sort(entries.begin(), entries.end(), [dirs_first](const auto& a, const auto& b) {
                                    if (a.second != b.second)
                                        return dirs_first ? a.second > b.second : a.second < b.second;
                                    return a.first < b.first;
                                });

                                // Custom nav inputs - Vr compatible gamepad controls, arrow key movements, or mouse only
                                ImGuiIO& io = ImGui::GetIO();
                                float scroll_y = ImGui::GetScrollY();
                                float scroll_max_y = ImGui::GetScrollMaxY();

                                bool no_mouse_input = io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f && !ImGui::IsMouseClicked(0);
                                int entry_count = entries.size();
                                if (no_mouse_input && entry_count > 0) {
                                    static int prev_selected = selected_entry;
                                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp)) {
                                        selected_entry = (selected_entry <= 0) ? entry_count - 1 : selected_entry - 1;
                                    }
                                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown)) {
                                        selected_entry = (selected_entry >= entry_count - 1) ? 0 : selected_entry + 1;
                                    }

                                    selected_entry = std::clamp(selected_entry, -1, entry_count - 1);
                                }

                                // Render entries
                                for (int i = 0; i < entries.size(); ++i) {
                                    const auto& [name, is_directory] = entries[i];
                                    std::string display_name = is_directory ? name + "/" : name;
                                    bool is_lua = !is_directory && std::filesystem::path(name).extension() == ".lua";


                                    bool is_selected = (i == selected_entry);

                                    ImGui::PushID(i);

                                    // Color styling
                                    if (is_selected) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f)); // Yellow
                                    } 

                                    // Selectable item
                                    if (ImGui::Selectable(display_name.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                                        selected_entry = i;
                                        std::filesystem::path entry_path = std::filesystem::path(current_path) / name;
                                        if (is_lua)
                                            script_path = entry_path.string();
                                        ImGui::SetScrollHereY(i / (entries.size() - 1));

                                        if (ImGui::IsMouseDoubleClicked(0)) {

                                            if (is_directory) {
                                                current_path = entry_path.string();
                                                selected_entry = -1;
                                                script_path.clear();
                                            } else if (is_lua){
                                                lua_text = read_file(script_path);

                                                text_editor.SetText(lua_text.data());
                                                open = false;
                                                ImGui::CloseCurrentPopup();                

                                            }
                                        }
                                    }

                                    if (is_selected && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                                                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))) {
                   
                                        std::filesystem::path entry_path = std::filesystem::path(current_path) / name;
                                        if (is_directory) {
                                            current_path = entry_path.string();
                                            ImGui::SetScrollHereY();
                                            selected_entry = -1;
                                            script_path.clear();
                                        } else if (is_lua) {
                                            lua_text = read_file(script_path);

                                            text_editor.SetText(lua_text.data());
                                            open = false;
                                            ImGui::CloseCurrentPopup();          

                                        }
                                    }

                                    ImGui::PopStyleColor(is_selected  ? 1 : 0);
                                    ImGui::PopID();
                                }

                            } catch (const std::filesystem::filesystem_error& e) {
                                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", e.what());
                            }
                            ImGui::EndChild();
                            ImGui::EndChild();
                            ImGui::EndPopup();
                        }

            ImGui::End();

       
         
    }

private:
    HWND m_wnd{};
    bool m_initialized{false};
    bool m_was_rendering_desktop{false};
    std::vector<std::string> lua_chunks {};

    std::recursive_mutex m_imgui_mutex{};
    bool full_editor{false};
};

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<ExamplePlugin> g_plugin{new ExamplePlugin()};
