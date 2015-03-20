# logtools
Tools to watch growing and cycling log files

## Goal

he logtools provide functionality missing in - say - tail and tee.
The basic idea is to pipe the new lines in a logfile through analysing
processing in real time.

There are some issues with `tail -f` and `tee`:

- When the system ist rebooted, the log analysis must continue from the last
point. A status must be maintained.
- when the log file is cycled and renamed, it does not grow any more. Thus,
it must be reopened.
- when the log file is truncated ("zeroed"), there must be "rewinding".
- the output must be distributed to processes, not files.

## Programs

As an answer, there are three tools here:

- `tailfd`, the "tail -f daemon"
- `teepee`, the "tee for processes"
- `tailfdx`, a daemon providing both functionality with a configuration file.

I use tailfd and teepee in a large mail system every day. They are not perfect, but an answer.

## Building

Being a really old attempt at autoconf, we have no `configure.ac` yet. I'll add
that later, promised, as there is no really special issue with that. Compile,
link and be done :-)

## License

(MIT License)

Copyright (c) 2013

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
'Software'), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

