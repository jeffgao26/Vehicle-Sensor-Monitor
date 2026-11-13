#!/usr/bin/env python3
"""Accumulate Codex token counters without retaining conversation text."""
import argparse
import json
import os
from pathlib import Path
import sqlite3

KEYS = ('input_tokens', 'cached_input_tokens', 'output_tokens', 'total_tokens')


def scan(path, project, warnings):
    session, matches, found = None, False, False
    previous = [0] * 4
    with path.open(encoding='utf-8') as stream:
        for line in stream:
            try:
                event = json.loads(line)
            except ValueError:
                continue  # An active writer may leave an incomplete last line.
            payload = event.get('payload') or {}
            if event.get('type') == 'session_meta':
                session = payload.get('id')
                cwd = payload.get('cwd')
                matches = bool(cwd) and Path(cwd).resolve() == project
            if not matches or event.get('type') != 'event_msg' or payload.get('type') != 'token_count':
                continue
            usage = (payload.get('info') or {}).get('total_token_usage')
            if not usage:
                continue
            values = [usage.get(k) for k in KEYS]
            if any(type(v) is not int or v < 0 for v in values):
                warnings.add('Unsupported counters; some usage was skipped.')
                continue
            if any(a < b for a, b in zip(values, previous)):
                warnings.add('Counters decreased; totals may be incomplete.')
            previous = [max(a, b) for a, b in zip(previous, values)]
            found = True
    return (session, previous) if matches and session and found else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path.cwd())
    parser.add_argument('--sessions', type=Path, default=Path(os.environ.get('CODEX_HOME', str(Path.home() / '.codex'))) / 'sessions')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    project = args.project.resolve()
    if not project.is_dir():
        parser.error('--project must exist')
    warnings, observations = set(), {}
    if not args.sessions.is_dir():
        warnings.add('Session log directory unavailable.')
    for path in sorted(args.sessions.rglob('*.jsonl')):
        try:
            result = scan(path, project, warnings)
        except (OSError, UnicodeError):
            warnings.add('Some session logs could not be read.')
            continue
        if result:
            session, values = result
            observations[session] = [max(a, b) for a, b in zip(observations.get(session, [0]*4), values)]
    ledger = project / '.usage-tracker' / 'usage.sqlite3'
    ledger.parent.mkdir(exist_ok=True)
    with sqlite3.connect(ledger, timeout=30) as db:
        db.execute('CREATE TABLE IF NOT EXISTS usage (session TEXT PRIMARY KEY, input INTEGER, cached INTEGER, output INTEGER, total INTEGER)')
        db.execute('BEGIN IMMEDIATE')
        before = db.execute('SELECT coalesce(sum(total),0) FROM usage').fetchone()[0]
        for session, values in observations.items():
            old = db.execute('SELECT input,cached,output,total FROM usage WHERE session=?', (session,)).fetchone()
            if old:
                if any(a < b for a, b in zip(values, old)):
                    warnings.add('Log counters below recorded usage; retained prior counters.')
                values = [max(a, b) for a, b in zip(values, old)]
            db.execute('INSERT OR REPLACE INTO usage VALUES (?,?,?,?,?)', (session, *values))
        totals = db.execute('SELECT count(*),coalesce(sum(input),0),coalesce(sum(cached),0),coalesce(sum(output),0),coalesce(sum(total),0) FROM usage').fetchone()
    if not observations:
        warnings.add('No matching usage snapshots; fresh usage unavailable.')
    report = dict(zip(('sessions', *KEYS), totals))
    report.update(added_tokens=totals[-1]-before, fresh_usage_available=bool(observations), warnings=sorted(warnings), project=str(project))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f'Tokens: +{report["added_tokens"]:,} since last update; {report["total_tokens"]:,} cumulative '
              f'(input {report["input_tokens"]:,}, including cached {report["cached_input_tokens"]:,}; output {report["output_tokens"]:,}).')
        print('Current final response is counted on the next refresh.')
        for warning in sorted(warnings):
            print('Warning: ' + warning)


if __name__ == '__main__':
    main()
