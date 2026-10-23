"""How many rows of the demo profile's clipboard history are not the demo's own (a Notepad or Paint copy): the system
clipboard's content WinShun took in at its start. Usage: python seedrows.py <clipboard.db>"""
import sqlite3
import sys

db = sqlite3.connect("file:" + sys.argv[1].replace("\\", "/") + "?mode=ro", uri=True)
print(db.execute("select count(*) from clips where source not in ('Notepad', 'Paint')").fetchone()[0])
