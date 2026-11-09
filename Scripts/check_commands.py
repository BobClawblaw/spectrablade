#!/usr/bin/env python3
"""Checks the command table in Source/MainComponent.cpp, so that what 0.14.0 and 0.14.1 shipped
cannot happen again without a test failing: twenty-five commands were in the enum but never
registered, and a command the manager does not know is left out of a menu without a word.

For every id in the command enum (up to the sentinel cmdCount, which the registration loop runs to):
  - it has a `case` in describeCommand (a name and a description) and one in perform;
  - it is reachable from a menu: the menu bar (getMenuForIndex), the app menu, or another
    addCommandItem, unless it is listed in MENU_LESS with the reason;
  - no two commands claim the same default key (JUCE's key map gives a key to one command only,
    so a clash makes a command lose its key silently). Keys written as `a ? b : c` count both ways.

Prints each finding and exits 1 when there is one; the test suite runs it. Standard library only.
"""
import pathlib
import re
import sys

SOURCE = pathlib.Path(__file__).resolve().parent.parent / 'Source' / 'MainComponent.cpp'

# Commands with no menu item, each with the reason it needs none.
MENU_LESS = {
}

MODIFIER_WORDS = {'cmd', 'shift', 'alt', 'ctrl'}


def function_body(text, header):
    """The text of the function whose definition starts with header, up to its closing brace at
    the start of a line."""
    start = text.index(header)
    end = text.index('\n}\n', start)
    return text[start:end]


def ids_in(text, known):
    return [i for i in dict.fromkeys(re.findall(r'\bcmd[A-Za-z0-9]+\b', text)) if i in known]


def split_top_level(expr, separator):
    """Splits expr at separator where no parentheses are open."""
    parts, depth, current = [], 0, ''
    for ch in expr:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == separator and depth == 0:
            parts.append(current)
            current = ''
        else:
            current += ch
    parts.append(current)
    return parts


def leaves(expr):
    """The outcomes of an expression with `?:` in it: each branch's text, stripped."""
    expr = expr.strip()
    if '?' not in expr:
        return [expr]
    condition, rest = expr.split('?', 1)
    branches = split_top_level(rest, ':')
    out = []
    for b in branches:
        out.extend(leaves(b))
    return out


def modifiers(expr):
    words = set(re.findall(r'\b(cmd|shift|alt|ctrl)\b', expr))
    rest = re.sub(r'\b(cmd|shift|alt|ctrl|0)\b|[|\s()]', '', expr)
    if rest:
        return None   # something this check doesn't read: skip the key
    return frozenset(words)


def key_name(expr):
    expr = expr.strip()
    m = re.fullmatch(r"'(.)'", expr) or re.fullmatch(r'juce::KeyPress::(\w+)', expr) or re.fullmatch(r'KeyPress::(\w+)', expr)
    return m.group(1) if m else None


def main():
    text = SOURCE.read_text()
    findings = []

    # The enum, up to the sentinel.
    first = text.index('cmdOpen')
    enum_start = text.rfind('{', 0, first)
    sentinel = re.search(r'\bcmdCount\b', text)
    if sentinel is None:
        findings.append('the command enum has no cmdCount sentinel at its end')
        return report(findings)
    ids = list(dict.fromkeys(re.findall(r'\bcmd[A-Za-z0-9]+\b', text[enum_start:sentinel.start()])))
    known = set(ids)

    # The registration loop runs to the sentinel.
    if not re.search(r'for \(int id = cmdOpen; id < cmdCount; \+\+id\)', text):
        findings.append('getAllCommands does not loop from cmdOpen to cmdCount')

    describe = function_body(text, 'void MainComponent::describeCommand')
    perform = function_body(text, 'bool MainComponent::perform')
    menus = function_body(text, 'juce::PopupMenu MainComponent::getMenuForIndex')
    in_menus = set(ids_in(menus, known))
    in_menus.update(re.findall(r'addCommandItem \(&commands, (cmd[A-Za-z0-9]+)\)', text))

    for i in ids:
        if not re.search(r'case\s+' + i + r'\b', describe):
            findings.append(f'{i} has no case in describeCommand (no name or description)')
        if not re.search(r'case\s+' + i + r'\b', perform):
            findings.append(f'{i} has no case in perform (nothing happens)')
        if i not in in_menus and i not in MENU_LESS:
            findings.append(f'{i} is in no menu (add it to a menu, or to MENU_LESS in this script with the reason)')

    for i in MENU_LESS:
        if i not in known:
            findings.append(f'MENU_LESS names {i}, which is not a command')
        elif i in in_menus:
            findings.append(f'MENU_LESS names {i}, which is in a menu after all')

    # Default keys, by case group: the labels at the start of each chunk of the switch own the
    # addDefaultKeypress calls in it.
    chunks = re.split(r'\n        case ', describe)[1:]
    claims = {}   # (key, modifiers) -> first group's ids

    for chunk in chunks:
        chunk = 'case ' + chunk
        label_text = re.match(r'(?:case\s+cmd[A-Za-z0-9]+\s*:\s*)+', chunk)
        group = tuple(re.findall(r'case\s+(cmd[A-Za-z0-9]+)', label_text.group(0))) if label_text else ()
        if not group:
            continue
        for call in re.findall(r'addDefaultKeypress \((.*?)\);', chunk, re.S):
            args = split_top_level(call, ',')
            if len(args) != 2:
                continue
            keys = [key_name(k) for k in leaves(args[0])]
            mods = [modifiers(m) for m in leaves(args[1])]
            if any(k is None for k in keys) or any(m is None for m in mods):
                continue   # written in a way this check does not read
            for k in keys:
                for m in mods:
                    owner = claims.setdefault((k, m), group)
                    if owner != group:
                        findings.append(f'default key {"+".join(sorted(m) + [k])} is claimed by {", ".join(owner)} and by {", ".join(group)}')

    return report(findings, len(ids), len(claims))


def report(findings, commands=0, keys=0):
    for f in findings:
        print('command table: ' + f)
    if findings:
        return 1
    print(f'command table: {commands} commands, each described, performed and in a menu; {keys} default keys, no two commands on one')
    return 0


if __name__ == '__main__':
    sys.exit(main())
