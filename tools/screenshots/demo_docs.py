"""Real (small) Office and PDF files for the screenshot demo folder, in place
of empty placeholders: real sizes in the search panel, thumbnails, and words
for the content search. Writes into each folder given on the command line:

    python tools/screenshots/demo_docs.py tools/screenshots/demo F:/Win顺演示
"""
import pathlib
import sys

import docx
import openpyxl
from docx.shared import Pt
from pptx import Presentation
from pptx.util import Inches, Pt as PPt
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

HERE = pathlib.Path(__file__).resolve().parent
FONTS = HERE.parents[1] / "resources" / "fonts"
COVER = HERE / "demo" / "年度报告 封面.png"  # the picture: made by hand once, not here
QUARTERS = (("第一季度", 980, 36), ("第二季度", 1150, 38), ("第三季度", 1260, 41), ("第四季度", 1420, 42))
REGIONS = (("华东", 486), ("华南", 452), ("华北", 198), ("西南", 124))


def word(path, title, paragraphs, table=None):
    d = docx.Document()
    style = d.styles["Normal"]
    style.font.name = "Microsoft YaHei"
    style.element.rPr.rFonts.set("{http://schemas.openxmlformats.org/wordprocessingml/2006/main}eastAsia", "微软雅黑")
    style.font.size = Pt(11)
    d.core_properties.title = title
    d.core_properties.author = "财务部"
    d.add_heading(title, 0)
    for p in paragraphs:
        if p.startswith("## "):
            d.add_heading(p[3:], 1)
        else:
            d.add_paragraph(p)
    if table:
        t = d.add_table(rows=len(table), cols=len(table[0]))
        t.style = "Light Grid Accent 1"
        for r, row in enumerate(table):
            for c, value in enumerate(row):
                t.cell(r, c).text = str(value)
    d.save(path)


def excel(path, sheets):
    wb = openpyxl.Workbook()
    wb.remove(wb.active)
    for name, rows in sheets:
        ws = wb.create_sheet(name)
        for row in rows:
            ws.append(row)
        ws.column_dimensions["A"].width = 22
        for cell in ws[1]:
            cell.font = openpyxl.styles.Font(bold=True)
    wb.save(path)


def slides(path):
    prs = Presentation()
    prs.slide_width, prs.slide_height = Inches(13.333), Inches(7.5)
    s = prs.slides.add_slide(prs.slide_layouts[6])
    s.shapes.add_picture(str(COVER), 0, 0, prs.slide_width, prs.slide_height)
    for title, lines in (
        ("全年回顾", ["全年营业收入 4810 万元，同比增长 15%", "新客户占四成，老客户复购率 62%"]),
        ("第三季度销售额", ["第三季度销售额 1260 万元，同比增长 18%", "华东区 486 万，首次超过华南"]),
        ("明年计划", ["老客户复购", "西南区新开两个办事处", "毛利率保持在 40% 以上"]),
    ):
        s = prs.slides.add_slide(prs.slide_layouts[1])
        s.shapes.title.text = title
        body = s.placeholders[1].text_frame
        body.text = lines[0]
        for line in lines[1:]:
            body.add_paragraph().text = line
        for p in body.paragraphs:
            p.font.size = PPt(28)
    prs.save(path)


def pdf(path):
    pdfmetrics.registerFont(TTFont("PuHuiTi", str(FONTS / "AlibabaPuHuiTi-3-55-Regular.ttf")))
    pdfmetrics.registerFont(TTFont("PuHuiTi-Bold", str(FONTS / "AlibabaPuHuiTi-3-85-Bold.ttf")))
    w, h = A4
    c = canvas.Canvas(str(path), pagesize=A4)
    c.setTitle("2025 年度报告")
    c.setFillColorRGB(0.09, 0.18, 0.36)
    c.rect(0, h - 120 * mm, w, 120 * mm, fill=1, stroke=0)
    c.setFillColorRGB(1, 1, 1)
    c.setFont("PuHuiTi-Bold", 40)
    c.drawString(20 * mm, h - 60 * mm, "2025 年度报告")
    c.setFont("PuHuiTi", 16)
    c.drawString(20 * mm, h - 75 * mm, "经营回顾与展望")
    c.setFillColorRGB(0.1, 0.1, 0.1)
    c.setFont("PuHuiTi-Bold", 18)
    c.drawString(20 * mm, h - 140 * mm, "一、全年回顾")
    c.setFont("PuHuiTi", 12)
    y = h - 152 * mm
    for line in ("全年营业收入 4180 万元，同比增长 11%。", "第三季度销售额 1070 万元，华南区仍居第一。",
                 "毛利率 37%，比上年提高两个百分点。"):
        c.drawString(20 * mm, y, line)
        y -= 8 * mm
    # a small bar chart
    for i, (name, value, _) in enumerate(QUARTERS):
        x = 20 * mm + i * 40 * mm
        c.setFillColorRGB(0.16, 0.45, 0.62)
        c.rect(x, 40 * mm, 24 * mm, value / 1420 * 60 * mm, fill=1, stroke=0)
        c.setFillColorRGB(0.2, 0.2, 0.2)
        c.drawString(x, 32 * mm, name)
    c.showPage()
    c.setFont("PuHuiTi-Bold", 18)
    c.drawString(20 * mm, h - 30 * mm, "二、明年计划")
    c.setFont("PuHuiTi", 12)
    c.drawString(20 * mm, h - 42 * mm, "华东区加大投入，老客户复购为下一年的重点。")
    c.save()


def main(folder):
    f = pathlib.Path(folder)
    word(f / "2026 年度报告.docx", "2026 年度报告", [
        "## 一、全年回顾",
        "全年营业收入 4810 万元，同比增长 15%；新客户占四成，老客户复购率 62%。",
        "## 二、第三季度",
        "第三季度销售额 1260 万元，同比增长 18%，环比增长 9%。华东区销售额 486 万，首次超过华南。",
        "## 三、明年计划",
        "老客户复购为重点；西南区新开两个办事处；毛利率保持在 40% 以上。",
    ], [("季度", "销售额（万元）", "毛利率")] + [(q, v, f"{m}%") for q, v, m in QUARTERS])
    word(f / "年度报告 致辞.docx", "年度报告 致辞", [
        "各位董事、同事：",
        "这一年，我们的营业收入第一次突破四千万元。感谢每一位同事的努力，也感谢客户的信任。",
        "新的一年，我们会把更多精力放在老客户身上，把服务做得更细。",
    ])
    excel(f / "年度报告 数据附录.xlsx", [
        ("季度数据", [("季度", "销售额（万元）", "毛利率")] + [(q, v, m / 100) for q, v, m in QUARTERS]),
        ("区域", [("区域", "第三季度销售额（万元）")] + list(REGIONS)),
    ])
    excel(f / "财务" / "年度报表汇总.xlsx", [
        ("汇总", [("指标", "全年", "第三季度")] + [("营业收入（万元）", 4810, 1260), ("第三季度销售额（万元）", None, 1260),
                                                    ("毛利率", 0.40, 0.41), ("费用率", 0.18, 0.17)]),
    ])
    slides(f / "年度报告 定稿 v3.pptx")
    pdf(f / "2025 年度报告.pdf")


for folder in sys.argv[1:]:
    main(folder)
