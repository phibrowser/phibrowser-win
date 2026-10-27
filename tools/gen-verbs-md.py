#!/usr/bin/env python3
"""Generates tools/phi-browser-skill/references/verbs.md from the C++ verb
registry.

The table in client/phi_verb_registry.cc is the ONE source of truth: it is
what the dispatcher gates on, what agentSpace.capabilities reports, and —
through this script — what the skill's reference says. Nothing here knows a
verb name; everything is read out of the C++.

    tools/gen-verbs-md.py            # rewrite verbs.md
    tools/gen-verbs-md.py --check    # regenerate and diff; exit 1 on drift
    tools/gen-verbs-md.py --stdout   # print, write nothing

Why parse the C++ rather than keep a data file that emits both: a data file
is a second place to forget. The table's shape is fixed by PhiVerbSpec (11
positional fields), which makes it mechanical to read, and a row that does
not parse is a hard error here rather than a silently missing verb in the
docs.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY_CC = os.path.join(ROOT, 'client', 'phi_verb_registry.cc')
REGISTRY_H = os.path.join(ROOT, 'client', 'phi_verb_registry.h')
OUT_MD = os.path.join(ROOT, 'tools', 'phi-browser-skill', 'references',
                      'verbs.md')

# PhiVerbSpec's fields, in declaration order.
SPEC_FIELDS = ['name', 'selector', 'scopes', 'required', 'flags', 'params',
               'prompts', 'deferred', 'handler', 'errors', 'doc']
# PhiParamField's fields, in declaration order.
PARAM_FIELDS = ['name', 'type', 'required', 'alias', 'doc']

CAPABILITY_NAMES = [
    ('kAgentSpace', 'agentSpace'),
    ('kUserSpaceRead', 'userSpaceRead'),
    ('kUserSpaceWrite', 'userSpaceWrite'),
    ('kDestructive', 'destructive'),
    ('kAppSettings', 'appSettings'),
    ('kHost', 'host'),
    ('kCredentialFill', 'credentialFill'),
    ('kCredentialReveal', 'credentialReveal'),
]

SCOPE_ALIASES = {
    'kScopeNone': [],
    'kScopeAgent': ['agent'],
    'kScopeShadow': ['shadow'],
    'kScopeUser': ['user'],
    'kScopeTask': ['agent', 'shadow'],
    'kScopeAny': ['agent', 'shadow', 'user'],
    'kTask': ['agent', 'shadow'],
    'kAny': ['agent', 'shadow', 'user'],
}

FLAG_NAMES = {
    'kVerbMutates': 'mutates the user\'s state (arms the agent drive context)',
    'kVerbDestructiveOnUser': 'needs `destructive` on a user window',
    'kVerbDefaultsToUser': 'no scope in the payload means the first user window',
    'kVerbHostForOutPath': 'needs `host` when the payload carries `outPath`',
    'kVerbDestructiveOnFolder': 'needs `destructive` when `guid` names a folder',
    'kReadDefault': 'no scope in the payload means the first user window',
    'kWriteDefault': ("mutates the user's state (arms the agent drive "
                      "context); no scope in the payload means the first "
                      "user window"),
    'kTargetWrite': "mutates the user's state (arms the agent drive context)",
    'kVerbPlain': None,
}

PARAM_TYPES = {
    'kParamString': 'string',
    'kParamInt': 'integer',
    'kParamBool': 'boolean',
    'kParamDouble': 'number',
    'kParamStringList': 'string[]',
    'kParamList': 'list',
    'kParamObject': 'object',
}


class ParseError(Exception):
    pass


def split_items(text):
    """Splits one brace-delimited initialiser body into its top-level items.

    Understands C++ string literals (and their escapes), nested braces and
    parentheses, and // comments — everything the table actually uses.
    """
    items, buf, depth, i, n = [], [], 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            buf.append(text[i:j + 1])
            i = j + 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                break
            continue
        if c in '{([':
            depth += 1
        elif c in '})]':
            depth -= 1
        elif c == ',' and depth == 0:
            items.append(''.join(buf).strip())
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    tail = ''.join(buf).strip()
    if tail:
        items.append(tail)
    return items


def unquote(literal):
    """Joins adjacent C++ string literals into one Python string."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', literal)
    if not parts:
        raise ParseError('not a string literal: %r' % literal)
    text = ''.join(parts)
    return (text.replace('\\"', '"').replace('\\n', '\n')
                .replace('\\\\', '\\'))


def find_block(text, header):
    """The body of `header` ... `};`, brace-balanced."""
    start = text.index(header) + len(header)
    depth, i, n = 1, start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                break
            continue
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    raise ParseError('unterminated block after %r' % header)


def parse_schemas(source):
    """Every `constexpr PhiParamField kX[] = {...};` in the file."""
    schemas = {}
    for match in re.finditer(r'constexpr PhiParamField (\w+)\[\] = \{',
                             source):
        name = match.group(1)
        body = find_block(source, match.group(0))
        fields = []
        for row in split_items(body):
            row = row.strip()
            if not row.startswith('{'):
                continue
            values = split_items(row[1:row.rindex('}')])
            if len(values) != len(PARAM_FIELDS):
                raise ParseError('%s: %d fields in %r' %
                                 (name, len(values), row))
            field = dict(zip(PARAM_FIELDS, values))
            fields.append({
                'name': unquote(field['name']),
                'type': PARAM_TYPES.get(field['type'], field['type']),
                'required': field['required'] == 'kRequired',
                'alias': unquote(field['alias']) if '"' in field['alias'] else '',
                'doc': unquote(field['doc']) if '"' in field['doc'] else '',
            })
        schemas[name] = fields
    return schemas


def parse_table(source):
    """The registry rows, each with the section comment above it."""
    header = 'static constexpr PhiVerbSpec kTable[] = {'
    body = find_block(source, header)
    rows, section = [], 'Verbs'
    # Section headings ride as `// --- Title ---` lines between rows; the
    # item splitter drops comments, so pick them up from the raw text and
    # attribute each to the rows that follow.
    marks = []
    for match in re.finditer(r'//\s*---+\s*(.*?)\s*-{3,}\s*$', body,
                             re.MULTILINE):
        marks.append((match.start(), match.group(1).strip()))
    offset = 0
    for item in split_items(body):
        item = item.strip()
        if not item.startswith('{'):
            continue
        at = body.index(item[:40], offset)
        offset = at + 1
        for pos, title in marks:
            if pos < at:
                section = title
        values = split_items(item[1:item.rindex('}')])
        if len(values) != len(SPEC_FIELDS):
            raise ParseError('%d fields in row starting %r' %
                             (len(values), item[:60]))
        row = dict(zip(SPEC_FIELDS, values))
        row['section'] = section
        rows.append(row)
    return rows


def parse_error_codes(header_source, cc_source):
    """The closed enum, paired with the wire names the .cc gives it."""
    enum = find_block(header_source, 'enum class PhiErrorCode : uint8_t {')
    symbols = []
    for line in enum.split('\n'):
        line = re.sub(r'//.*', '', line).strip().rstrip(',')
        if not line or '=' in line:
            continue
        symbols.append(line)
    names = find_block(cc_source,
                       'constexpr std::string_view kErrorCodeNames[] = {')
    wires = [unquote(v) for v in split_items(names) if '"' in v]
    if len(symbols) != len(wires):
        raise ParseError('%d enum codes vs %d wire names' %
                         (len(symbols), len(wires)))
    return list(zip(symbols, wires))


def parse_reason_codes(cc_source):
    body = find_block(cc_source, 'constexpr ReasonCode kReasonCodes[] = {')
    out = []
    for row in split_items(body):
        row = row.strip()
        if not row.startswith('{'):
            continue
        reason, code = split_items(row[1:row.rindex('}')])
        out.append((unquote(reason), code.split('::')[-1]))
    return out


def capabilities_of(expr):
    bits = [token.strip() for token in expr.split('|')]
    names = [wire for symbol, wire in CAPABILITY_NAMES if symbol in bits]
    return names or ['—']


def scopes_of(expr):
    names = []
    for token in expr.split('|'):
        names.extend(SCOPE_ALIASES.get(token.strip(), []))
    seen = []
    for name in names:
        if name not in seen:
            seen.append(name)
    return seen


def flags_of(expr):
    notes = []
    for token in expr.split('|'):
        note = FLAG_NAMES.get(token.strip(), token.strip())
        if note and note not in notes:
            notes.append(note)
    return notes


def escape(text):
    return text.replace('|', '\\|')


def render(rows, schemas, error_codes, reason_codes):
    out = []
    w = out.append
    w('# Phi control verbs')
    w('')
    w('<!-- GENERATED by tools/gen-verbs-md.py from '
      'client/phi_verb_registry.cc — do not edit by hand. -->')
    w('')
    w('Every verb the client serves on the `/phi-agent` control channel, '
      'read straight out of the C++ registry the dispatcher gates on. If a '
      'verb is not here it is not callable; if a field is not in a verb\'s '
      'payload table the verb does not read it.')
    w('')

    w('## The wire')
    w('')
    w('The channel speaks one of two protocols, chosen on the WebSocket '
      'upgrade and fixed for the connection\'s life. Send '
      '`X-Phi-Protocol: 2` to get the typed wire; the 101 echoes '
      '`X-Phi-Protocol: 2` back when the client understood it. Without the '
      'header (or without the echo) the connection speaks protocol 1, '
      'byte-compatible with every earlier build.')
    w('')
    w('**Protocol 1** — the payload and the reply body are JSON *strings*:')
    w('')
    w('```json')
    w('--> {"id":7,"type":"agentSpace.spaces.list","payloadJson":"{}"}')
    w('<-- {"id":7,"responseJson":"{\\"ok\\":true,\\"spaces\\":[…]}"}')
    w('<-- {"id":8,"responseJson":"{\\"ok\\":false,\\"error\\":\\"unknown_task\\"}"}')
    w('<-- {"event":"agentSpace.ownershipChanged","payloadJson":"{…}"}')
    w('```')
    w('')
    w('**Protocol 2** — objects, a closed error code, and no double '
      'encoding:')
    w('')
    w('```json')
    w('--> {"id":7,"type":"agentSpace.spaces.list","payload":{}}')
    w('<-- {"id":7,"ok":true,"result":{"spaces":[…]}}')
    w('<-- {"id":8,"ok":false,"error":{"code":"unknown_task"}}')
    w('<-- {"id":9,"ok":false,"error":{"code":"invalid_payload",'
      '"message":"missing required field `taskId`"}}')
    w('<-- {"event":"agentSpace.ownershipChanged","payload":{…}}')
    w('```')
    w('')
    w('A verb that answers a finer token than its code keeps the token: '
      'protocol 1 puts it in `error`, protocol 2 puts the closed code in '
      '`error.code` and the token in `error.details.reason`. Any other '
      'fields a refusal carries ride alongside it in `error.details`.')
    w('')
    w('A scoped verb whose `taskId`, `windowId` or `spaceId` names nothing '
      'live is refused with `unknown_task`, `window_not_open` or '
      '`space_not_open`, whether or not its row below lists the code.')
    w('')

    w('## Error codes')
    w('')
    w('The closed set. Protocol 2 answers only these in `error.code`.')
    w('')
    w('| code | enum |')
    w('| --- | --- |')
    for symbol, wire in error_codes:
        w('| `%s` | `PhiErrorCode::%s` |' % (wire, symbol))
    w('')
    w('### Finer tokens')
    w('')
    w('These are what protocol 1 puts in `error`; protocol 2 reports the '
      'code beside them and repeats the token in `error.details.reason`.')
    w('')
    w('| token | code |')
    w('| --- | --- |')
    for reason, code in reason_codes:
        wire = dict(error_codes).get(code, code)
        w('| `%s` | `%s` |' % (reason, wire))
    w('')

    sections = []
    for row in rows:
        if not sections or sections[-1][0] != row['section']:
            sections.append((row['section'], []))
        sections[-1][1].append(row)

    w('## Verbs')
    w('')
    w('%d rows, %d distinct names.' %
      (len(rows), len({unquote(r['name']) for r in rows})))
    w('')
    for title, group in sections:
        w('### %s' % title)
        w('')
        for row in group:
            name = unquote(row['name'])
            selector = unquote(row['selector']) if '"' in row['selector'] else ''
            heading = '`%s`' % name
            if selector:
                heading += ' — the row claimed when the payload carries `%s`' \
                           % selector
            w('#### %s' % heading)
            w('')
            w(unquote(row['doc']))
            w('')
            bullets = []
            caps = capabilities_of(row['required'])
            bullets.append('**Capabilities**: %s' %
                           ', '.join('`%s`' % c for c in caps))
            scopes = scopes_of(row['scopes'])
            if scopes:
                bullets.append(
                    '**Scope**: %s — aimed with `taskId`, `windowId` or '
                    '`spaceId`. A verb aimed at an agent or shadow window of '
                    'the caller\'s OWN task needs only `agentSpace`.' %
                    ', '.join(scopes))
            else:
                bullets.append('**Scope**: app-wide.')
            for note in flags_of(row['flags']):
                bullets.append(note[0].upper() + note[1:] + '.')
            if row['prompts'] == 'true':
                bullets.append('Shows the user a prompt; serialised so two '
                               'never stack.')
            if row['deferred'] == 'true':
                bullets.append('Answers asynchronously — the reply arrives '
                               'when the work lands.')
            errors = unquote(row['errors']) if '"' in row['errors'] else ''
            if errors.strip():
                bullets.append('**Errors**: %s' %
                               ', '.join('`%s`' % e for e in errors.split()))
            for bullet in bullets:
                w('- %s' % bullet)
            w('')
            schema = row['params']
            fields = schemas.get(schema.strip(), []) if schema.strip() != '{}' \
                else []
            if fields:
                w('| field | type | required | also accepts | meaning |')
                w('| --- | --- | --- | --- | --- |')
                for field in fields:
                    aliases = ', '.join('`%s`' % a
                                        for a in field['alias'].split('|')
                                        if a)
                    w('| `%s` | %s | %s | %s | %s |' %
                      (field['name'], field['type'],
                       'yes' if field['required'] else 'no',
                       aliases or '—', escape(field['doc'])))
            else:
                w('*No payload fields.*')
            w('')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='regenerate and diff instead of writing')
    parser.add_argument('--stdout', action='store_true',
                        help='print the document and write nothing')
    args = parser.parse_args()

    cc = open(REGISTRY_CC, encoding='utf-8').read()
    h = open(REGISTRY_H, encoding='utf-8').read()
    try:
        schemas = parse_schemas(cc)
        rows = parse_table(cc)
        error_codes = parse_error_codes(h, cc)
        reason_codes = parse_reason_codes(cc)
    except (ParseError, ValueError) as err:
        print('gen-verbs-md: could not read the registry: %s' % err,
              file=sys.stderr)
        return 2
    if not rows:
        print('gen-verbs-md: the table parsed to nothing', file=sys.stderr)
        return 2
    text = render(rows, schemas, error_codes, reason_codes)

    if args.stdout:
        sys.stdout.write(text)
        return 0
    if args.check:
        try:
            current = open(OUT_MD, encoding='utf-8').read()
        except FileNotFoundError:
            current = None
        if current == text:
            print('verbs.md is up to date (%d rows).' % len(rows))
            return 0
        print('verbs.md is STALE — run tools/gen-verbs-md.py.',
              file=sys.stderr)
        if current is not None:
            import difflib
            for line in difflib.unified_diff(
                    current.split('\n'), text.split('\n'),
                    'references/verbs.md', 'generated', lineterm=''):
                print(line, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    open(OUT_MD, 'w', encoding='utf-8').write(text)
    print('wrote %s (%d rows).' % (os.path.relpath(OUT_MD, ROOT), len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
