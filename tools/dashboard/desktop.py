"""Tk 視窗。序列埠、記錄與狀態由 Session 負責。"""

from __future__ import annotations

from pathlib import Path
from queue import Empty, SimpleQueue
import threading
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

from view_model import CHART_MS, DashboardModel, swa_display

BG = '#0d1724'
PANEL = '#162435'
PLOT = '#101c2b'
GRID = '#34485e'
TEXT = '#edf4fb'
MUTED = '#a4b8cc'
LEFT = '#65d8ee'
RIGHT = '#f6b66c'
GOOD = '#86dfa6'
WARN = '#ffd080'
DANGER = '#ff9c9c'
TICK_MS = 100


def show(value, fmt=''):
    return '—' if value is None else format(value, fmt)


class DashboardWindow:
    def __init__(self, root: tk.Tk, model: DashboardModel, stop: threading.Event) -> None:
        self.root, self.model, self.stop = root, model, stop
        self.jobs: SimpleQueue[str] = SimpleQueue()
        self.command_pending = False
        self.command_message = ''
        self.rendered_history = {}
        self.timer = None

        root.title('EspFS 儀表板')
        root.geometry(f'{min(1230, root.winfo_screenwidth() - 40)}x{min(900, root.winfo_screenheight() - 80)}')
        root.minsize(800, 660)
        root.configure(background=BG)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.bind('<Control-c>', lambda _: self.close())
        style = ttk.Style(root)
        style.theme_use('clam')
        style.configure('.', background=PANEL, foreground=TEXT, fieldbackground=PLOT)
        style.configure('TButton', padding=(10, 5), background=GRID)
        style.map('TButton', foreground=[('disabled', MUTED)], background=[('active', '#466482')])
        style.configure('TNotebook.Tab', padding=(14, 5))
        style.map('TNotebook.Tab', background=[('selected', GRID)])

        self.top = self._label(root, font=('TkDefaultFont', 13, 'bold'), background=BG)
        self.top.pack(fill='x', padx=16, pady=(12, 4))
        self.log = self._label(root, foreground=MUTED, background=BG)
        self.log.pack(fill='x', padx=16, pady=(0, 7))
        self.banners = tk.Frame(root, bg=BG)
        self.banners.pack(fill='x', padx=14)
        self.warnings = {}
        for key, fg, bg in (('latched', '#fff3f3', '#852a32'), ('reboot', WARN, '#513817'),
                            ('dump', '#d9f8ff', '#13516a'), ('connection', WARN, '#513817')):
            banner = self._label(self.banners, foreground=fg, background=bg, padx=10, pady=5,
                                 font=('TkDefaultFont', 11, 'bold'))
            self.warnings[key] = banner
            banner.grid(row=len(self.warnings) - 1, column=0, sticky='ew', pady=(0, 4))
        self.banners.columnconfigure(0, weight=1)

        main = ttk.Panedwindow(root, orient='vertical')
        main.pack(fill='both', expand=True, padx=14, pady=(3, 14))
        telemetry = tk.Frame(main, bg=BG)
        main.add(telemetry, weight=3)
        lower = ttk.Notebook(main)
        main.add(lower, weight=2)
        telemetry.columnconfigure(0, weight=1, uniform='card')
        telemetry.columnconfigure(1, weight=1, uniform='card')
        telemetry.columnconfigure(2, weight=2, uniform='card')
        telemetry.rowconfigure(0, weight=1)

        status = self._card(telemetry, 0, '狀態')
        self.system = self._label(status, font=('TkDefaultFont', 16, 'bold'))
        self.system.pack(fill='x', padx=10, pady=(3, 2))
        self.rx = self._label(status, justify='left', foreground=MUTED)
        self.rx.pack(fill='x', padx=10, pady=(0, 6))
        commands = ttk.Frame(status)
        commands.pack(fill='x', padx=10)
        self.dump_button = ttk.Button(commands, text='dump', command=lambda: self._command('dump'))
        self.erase_button = ttk.Button(commands, text='erase', command=lambda: self._command('erase'))
        self.dump_button.pack(side='left', padx=(0, 6))
        self.erase_button.pack(side='left')
        self.command_status = self._label(status, foreground=MUTED, wraplength=190, justify='left')
        self.command_status.pack(fill='x', padx=10, pady=5)

        remote = self._card(telemetry, 1, '遙控')
        self.stick = self._canvas(remote, 132)
        self.channels = self._label(remote, justify='left')
        self.channels.pack(fill='x', padx=10, pady=(3, 0))
        self.swa = self._label(remote, font=('TkDefaultFont', 10, 'bold'))
        self.swa.pack(fill='x', padx=10)
        self._label(remote, text='SWA 撥上＝關 · 撥下＝開\n中點 1500 · 死區 1460–1540',
                    justify='left', foreground=MUTED).pack(fill='x', padx=10, pady=(0, 5))

        wheels = self._card(telemetry, 2, '輪子  /  r/min')
        self.wheel_plot = self._canvas(wheels, 116)
        self.wheel_values = {}
        table = tk.Frame(wheels, bg=PANEL)
        table.pack(fill='x', padx=10, pady=3)
        for column, name in enumerate(('', '左 L', '右 R')):
            table.columnconfigure(column, weight=1)
            self._label(table, text=name, foreground=(MUTED, LEFT, RIGHT)[column]).grid(row=0, column=column, sticky='ew')
        for row, name in enumerate(('實際轉速', '目標轉速', '電流 A', 'error_code', 'motor_state'), 1):
            self._label(table, text=name, foreground=MUTED).grid(row=row, column=0, sticky='ew')
            for column, side in enumerate('LR', 1):
                label = self._label(table)
                label.grid(row=row, column=column, sticky='ew')
                self.wheel_values[side, row] = label

        chart = ttk.Frame(lower)
        lower.add(chart, text='最近 60 秒')
        self.rpm = self._canvas(chart, 100)
        self._legend(chart, (('目標 L / R（虛線）', MUTED), ('實際 L', LEFT), ('實際 R', RIGHT)))
        self.rc = self._canvas(chart, 77)
        self._legend(chart, (('油門 CH2', LEFT), ('轉向 CH4', RIGHT)))
        event_tab, self.event_text = self._text_tab(lower, '事件')
        raw_tab, self.raw_text = self._text_tab(lower, '原始輸出（最近 200 行）')
        self.event_text.tag_configure('dump', foreground=MUTED)
        self.event_text.tag_configure('error', foreground=DANGER)
        self.event_text.tag_configure('warn', foreground=WARN)
        self.show_dump = tk.BooleanVar(value=True)
        ttk.Checkbutton(event_tab, text='顯示傾倒事件', variable=self.show_dump).grid(row=1, column=0, sticky='w', padx=8, pady=3)
        self.raw_text.tag_configure('dump', foreground=MUTED)
        self.refresh()

    @staticmethod
    def _label(parent, **opts):
        return tk.Label(parent, **({'bg': PANEL, 'fg': TEXT, 'anchor': 'w'} | opts))

    def _card(self, parent, column, title):
        card = tk.Frame(parent, bg=PANEL, highlightbackground=GRID, highlightthickness=1)
        card.grid(row=0, column=column, sticky='nsew', padx=(0, 7) if column < 2 else 0)
        self._label(card, text=title, foreground=MUTED, font=('TkDefaultFont', 10, 'bold')).pack(fill='x', padx=10, pady=(6, 2))
        return card

    @staticmethod
    def _canvas(parent, height):
        canvas = tk.Canvas(parent, height=height, width=160, bg=PLOT, highlightthickness=0)
        canvas.pack(fill='both', expand=True, padx=8, pady=2)
        return canvas

    def _legend(self, parent, entries):
        row = ttk.Frame(parent)
        row.pack(fill='x', padx=8)
        for name, color in entries:
            self._label(row, text=name, foreground=color).pack(side='left', padx=(4, 12))

    @staticmethod
    def _text_tab(notebook, title):
        tab = ttk.Frame(notebook)
        notebook.add(tab, text=title)
        text = tk.Text(tab, height=8, bg=PLOT, fg=TEXT, wrap='none', state='disabled', borderwidth=0)
        scrollbar = ttk.Scrollbar(tab, orient='vertical', command=text.yview)
        horizontal = ttk.Scrollbar(tab, orient='horizontal', command=text.xview)
        text.configure(yscrollcommand=scrollbar.set, xscrollcommand=horizontal.set)
        text.grid(row=0, column=0, sticky='nsew', padx=(4, 0), pady=(4, 0))
        scrollbar.grid(row=0, column=1, sticky='ns')
        horizontal.grid(row=2, column=0, sticky='ew')
        tab.rowconfigure(0, weight=1)
        tab.columnconfigure(0, weight=1)
        return tab, text

    def _banner(self, key, content):
        widget = self.warnings[key]
        if content:
            widget.configure(text=content)
            widget.grid()
        else:
            widget.grid_remove()

    def refresh(self):
        self.model.refresh()
        self.render()
        self.timer = self.root.after(TICK_MS, self.refresh)

    def render(self):
        model, s = self.model, self.model.state
        rx, control = s['rx'], s['control']
        uptime = f'{s["uptime_ms"]:,} ms' if s['uptime_ms'] is not None else '—'
        self.top.configure(text=f'{model.connection_label}    {model.port or "—"}    '
                                f'uptime {uptime}    開機次數 {s["boot_count"]}')
        logfile = Path(s['log_file']).name if s['log_file'] else '—'
        self.log.configure(text=f'log {logfile}（{s["log_lines"]:,} 行）')
        self._banner('connection', s['connection']['message'] if model.connection_status not in ('replay', 'replay_done') else '')
        latched = f'LATCHED · 已鎖定停機    reason={s["latched_reason"] or "—"}    detail={s["latched_detail"] or "—"}'
        self._banner('latched', '\n'.join(filter(None, (latched, model.probe_hint))) if s['latched'] else '')
        self._banner('reboot', model.reboot_notice)
        self._banner('dump', f'傾倒中 · 面板和曲線暫停更新    已收 {s["dump_lines"]:,} 行' if model.dumping else '')
        self.system.configure(text=s['sys_state'] or '—', foreground=DANGER if s['latched'] else TEXT)
        self.rx.configure(text=f'reason  {s["reason"] or "—"}\n接收器  {rx.get("status", "—")}\n'
                               f'frame_age  {show(rx.get("frame_age_ms"))} ms\n'
                               f'values_unchanged  {show(rx.get("values_unchanged_ms"))} ms'
                               + ('\nDRY 版：不會送速度' if s['dry'] else ''))
        self.channels.configure(text=f'CH2 油門  {show(rx.get("ch2"))}    CH4 轉向  {show(rx.get("ch4"))}')
        swa, color = swa_display(rx.get('ch7') if rx.get('status') == 'RECEIVING' else None)
        self.swa.configure(text=f'CH7 SWA  {show(rx.get("ch7"))}  {swa}', fg={'muted': MUTED, 'good': GOOD, 'warn': WARN}[color])
        self._draw_stick(rx)
        self._draw_wheels(control)
        self._draw_chart(self.rpm, (-80, 80), (('tgtL', LEFT, (4, 3)), ('tgtR', RIGHT, (4, 3)),
                                             ('actL', LEFT, ()), ('actR', RIGHT, ())), (80, 0, -80))
        self._draw_chart(self.rc, (1000, 2000), (('ch2', LEFT, ()), ('ch4', RIGHT, ())), (2000, 1500, 1000))
        self._draw_history()
        try:
            self.command_message = self.jobs.get_nowait()
            self.command_pending = False
        except Empty:
            pass
        enabled = s['commands_enabled'] and not self.command_pending
        for button in (self.dump_button, self.erase_button):
            button.state(['!disabled'] if enabled else ['disabled'])
        self.command_status.configure(text='送出中…' if self.command_pending else
                                      (s['command_block_reason'] or self.command_message))

    def _draw_stick(self, rx):
        c = self.stick
        c.delete('all')
        size = max(30, min(c.winfo_width(), c.winfo_height()) - 16)
        cx, cy = c.winfo_width() / 2, c.winfo_height() / 2
        half = size / 2
        dead = size * 0.04
        c.create_rectangle(cx-half, cy-half, cx+half, cy+half, outline=GRID)
        c.create_rectangle(cx-dead, cy-half, cx+dead, cy+half, fill=GRID, outline='')
        c.create_rectangle(cx-half, cy-dead, cx+half, cy+dead, fill=GRID, outline='')
        c.create_line(cx, cy-half, cx, cy+half, fill=MUTED, dash=(2, 3))
        c.create_line(cx-half, cy, cx+half, cy, fill=MUTED, dash=(2, 3))
        ch2, ch4 = rx.get('ch2'), rx.get('ch4')
        if rx.get('status') == 'RECEIVING' and ch2 is not None and ch4 is not None:
            x = cx + (min(2000, max(1000, ch4)) - 1500) * size / 1000
            y = cy - (min(2000, max(1000, ch2)) - 1500) * size / 1000
            c.create_oval(x-5, y-5, x+5, y+5, fill=LEFT, outline=TEXT)

    def _draw_wheels(self, control):
        c = self.wheel_plot
        c.delete('all')
        start, end = 26, max(46, c.winfo_width() - 22)
        height = max(57, c.winfo_height())

        def x(rpm):
            return start + (min(80, max(-80, rpm)) + 80) * (end - start) / 160

        for rpm in (-80, -72, -60, 0, 60, 72, 80):
            c.create_line(x(rpm), 7, x(rpm), height-19, fill=DANGER if abs(rpm) == 72 else GRID,
                          dash=(3, 3) if abs(rpm) == 60 else ())
            if abs(rpm) != 72:
                c.create_text(x(rpm), height-10, text=str(rpm), fill=MUTED)
        for idx, (side, color) in enumerate((('L', LEFT), ('R', RIGHT)), 1):
            y = 7 + (height - 26) * idx / 3
            c.create_text(14, y, text=side, fill=color)
            value, stale = self.model.feedback(f'actual_velocity_{side}')
            target = control.get(f'tgt{side}')
            if value is not None:
                c.create_rectangle(min(x(0), x(value)), y-4, max(x(0), x(value)), y+4,
                                   fill=MUTED if stale else color, outline='')
            if target is not None:
                tx = x(target)
                c.create_polygon(tx, y+4, tx-4, y+11, tx+4, y+11, fill=TEXT)
            fields = [(value, stale, '.1f'), (target, False, '.1f')]
            for name, fmt in (('actual_torque', '.1f'), ('error_code', '04X'), ('motor_state', '.0f')):
                val, old = self.model.feedback(f'{name}_{side}')
                fields.append((int(val) if val is not None and fmt == '04X' else val, old, fmt))
            for row, (val, old, fmt) in enumerate(fields, 1):
                prefix = '0x' if val is not None and fmt == '04X' else ''
                self.wheel_values[side, row].configure(text=prefix + show(val, fmt) + ('（舊）' if old else ''),
                                                        fg=DANGER if row == 4 and val else MUTED if old else TEXT)

    def _draw_chart(self, canvas, bounds, series, ticks):
        canvas.delete('all')
        left, right = 46, canvas.winfo_width() - 12
        top, bottom = 10, canvas.winfo_height() - 20
        if right <= left or bottom <= top:
            return
        low, high = bounds
        now = self.model.state['uptime_ms'] or 0

        def y(value):
            return bottom - (min(high, max(low, value)) - low) * (bottom-top) / (high-low)

        for value in ticks:
            canvas.create_line(left, y(value), right, y(value), fill=GRID)
            canvas.create_text(left-5, y(value), text=str(value), fill=MUTED, anchor='e')
        for age in (60, 45, 30, 15, 0):
            x = right - age * (right-left) / 60
            canvas.create_line(x, top, x, bottom, fill=GRID)
            canvas.create_text(x, bottom+10, text=f'−{age}s' if age else '0s', fill=MUTED)
        for key, color, dash in series:
            segment = []
            for sample in self.model.samples:
                value = sample.get(key)
                if value is None:
                    self._plot_segment(canvas, segment, color, dash)
                    segment = []
                    continue
                x = right - (now-sample['ms']) * (right-left) / CHART_MS
                segment.extend((x, y(value)))
            self._plot_segment(canvas, segment, color, dash)

    @staticmethod
    def _plot_segment(canvas, points, color, dash):
        if len(points) >= 4:
            canvas.create_line(*points, fill=color, dash=dash, width=2)
        elif points:
            x, y = points
            canvas.create_oval(x-2, y-2, x+2, y+2, outline='', fill=color)

    def _draw_history(self):
        events, raw = self.model.events, self.model.raw
        show_dump = self.show_dump.get()
        event_key = (events[-1]['seq'] if events else 0, show_dump)
        if self.rendered_history.get('events') != event_key:
            self.rendered_history['events'] = event_key
            entries = []
            for event in events:
                dump = event['dump']
                if dump and not show_dump:
                    continue
                board = f'{event["ms"] / 1000:.3f}s' if event['ms'] is not None else '—'
                mark = ' [傾倒]' if dump else ''
                entries.append((f'{event["host"]}  {board:>10}  {event["text"]}{mark}\n', 'dump' if dump else event['level']))
            self._replace_text(self.event_text, entries)
        raw_key = raw[-1]['seq'] if raw else 0
        if self.rendered_history.get('raw') != raw_key:
            self.rendered_history['raw'] = raw_key
            self._replace_text(self.raw_text, [(f'{line["text"]}\n', 'dump' if line['dump'] else '') for line in raw])

    @staticmethod
    def _replace_text(widget, entries):
        start, end = widget.yview()
        widget.configure(state='normal')
        widget.delete('1.0', 'end')
        for line, tag in entries:
            widget.insert('end', line, (tag,) if tag else ())
        widget.configure(state='disabled')
        if end >= 0.99:
            widget.see('end')
        else:
            widget.yview_moveto(start)

    def _command(self, name):
        if self.command_pending or not self.model.state['commands_enabled']:
            return
        token = None
        if name == 'erase':
            if not messagebox.askyesno('清除板上紀錄', '確定清除板上紀錄？', parent=self.root):
                return
            token = simpledialog.askstring('確認清除', '輸入 ERASE 確認', parent=self.root)
            if token != 'ERASE':
                return
        self.command_pending = True
        threading.Thread(target=self._send, args=(name, token), daemon=True).start()

    def _send(self, name, token):
        _, result = self.model.session.send_command(name, token)
        self.jobs.put(f'{name} · {result}')

    def close(self):
        self.stop.set()
        if self.timer is not None:
            self.root.after_cancel(self.timer)
            self.timer = None
        self.root.destroy()
