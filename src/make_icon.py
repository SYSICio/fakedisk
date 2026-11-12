# 生成 fakedisk 程序图标 icon.ico（16~256 多尺寸）
from PIL import Image, ImageDraw

def make_icon(size):
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 参数按 256 基准缩放
    def P(v):
        return v * s / 256
    # 硬盘主体（圆角矩形，深蓝）
    body = (int(P(28)), int(P(52)), int(P(228)), int(P(216)))
    r = int(P(26))
    d.rounded_rectangle(body, radius=r, fill=(23, 82, 148, 255))
    # 顶部高光
    d.rounded_rectangle((int(P(36)), int(P(60)), int(P(220)), int(P(92))),
                        radius=int(P(10)), fill=(66, 134, 210, 255))
    # 中缝/面板
    d.rectangle((int(P(52)), int(P(104)), int(P(204)), int(P(108))), fill=(14, 58, 110, 255))
    # 盘片（同心圆，蓝灰）
    cx, cy, cr = int(P(128)), int(P(150)), int(P(56))
    d.ellipse((cx - cr, cy - cr, cx + cr, cy + cr), outline=(178, 211, 245, 255), width=int(P(6)))
    d.ellipse((cx - int(P(34)), cy - int(P(34)), cx + int(P(34)), cy + int(P(34))),
              outline=(178, 211, 245, 255), width=int(P(5)))
    d.ellipse((cx - int(P(12)), cy - int(P(12)), cx + int(P(12)), cy + int(P(12))),
              fill=(255, 255, 255, 220))
    # 磁头臂
    d.line((int(P(86)), int(P(118)), int(P(150)), int(P(132))), fill=(220, 235, 250, 255), width=int(P(6)))
    d.polygon([(int(P(148)), int(P(126)),), (int(P(162)), int(P(136))), (int(P(150)), int(P(140)))],
              fill=(220, 235, 250, 255))
    # 指示灯（绿）
    lx, ly, lr = int(P(196)), int(P(192)), int(P(9))
    d.ellipse((lx - lr, ly - lr, lx + lr, ly + lr), fill=(80, 220, 120, 255))
    d.ellipse((lx - lr + int(P(2)), ly - lr + int(P(2)), lx, ly), fill=(180, 255, 200, 255))
    # 底部阴影
    return img

sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
imgs = [make_icon(w).resize((w, h), Image.LANCZOS) for w, h in sizes]
imgs[0].save(r"C:\Users\ncyc\Desktop\fakedisk\src\icon.ico",
              format="ICO", sizes=[(w, h) for w, h in sizes], append_images=imgs[1:])
print("icon.ico written:", len(imgs), "sizes")
