"""Audit visible labels that are paired with rendered controller glyphs."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LITERAL = r'"(?:[^"\\]|\\.)*"'
PREFIX = re.compile(r"^(?:círculo|circulo|circle|cruz|cross|x|cuadrado|square|triángulo|triangulo|triangle|options|l1|r1)\b",
                    re.IGNORECASE)
TRANSLATION = re.compile(r"\bX4_TR_\w+\b")
lang = (ROOT / "src/ui/lang.c").read_text(encoding="utf-8")
translations = {}
for name, label in re.findall(r"\[(X4_TR_\w+)\]\s*=\s*(" + LITERAL + r")", lang):
    translations.setdefault(name, []).append(label[1:-1])


def call_arguments(source, name):
    for start in re.finditer(r"\b" + name + r"\s*\(", source):
        depth = 0
        previous = start.end()
        values = []
        for token in re.finditer(LITERAL + r"|[(),]", source[previous:]):
            symbol = token.group()
            at = start.end() + token.start()
            if symbol == ")" and depth == 0:
                values.append(source[previous:at].strip())
                yield source.count("\n", 0, start.start()) + 1, values
                break
            if symbol == "," and depth == 0:
                values.append(source[previous:at].strip())
                previous = at + 1
            if symbol == "(":
                depth += 1
            elif symbol == ")":
                depth -= 1


def audit(expression, context):
    literals = [label[1:-1] for label in re.findall(LITERAL, expression)]
    for name in TRANSLATION.findall(expression):
        literals.extend(translations[name])
    assert literals, (context, expression)
    for label in literals:
        assert not PREFIX.match(label.lstrip()), (context, label)


aliases = {
    ("loading_screen.c", "cancel"): '"Cancelar", "Cancel"',
    ("picture_settings.c", "save"): "X4_TR_ACCESS_CONTINUE X4_TR_ACCESS_SAVE",
    ("game_page.c", "select"): "X4_TR_CAT_HOME_PLAY X4_TR_APPLE_HINT_SELECT",
}
checked = 0
for path in (ROOT / "src").rglob("*.c"):
    source = path.read_text(encoding="utf-8")
    for line, args in call_arguments(source, "x4_apple_draw_footer_hint"):
        if args[0].startswith("uint32_t"):
            continue
        assert len(args) == 5, (path, line, args)
        audit(aliases.get((path.name, args[4]), args[4]), f"{path.name}:{line}")
        checked += 1

# Standalone glyphs adjacent to a translated label or attached to a pill.
audit("X4_TR_APPLE_HINT_SEARCH X4_TR_CAT_RECENT_RETRY", "catalog_gui.c standalone")
audit("X4_TR_CLOUD_TIME_TRY X4_TR_CANCEL_HINT", "screen.c cloud allowance")
screen = (ROOT / "src/ui/screen.c").read_text(encoding="utf-8")
for line, args in call_arguments(screen, "phase2_back"):
    if not args[0].startswith("uint32_t"):
        audit(args[1], f"screen.c:{line}")
assert checked >= 40
print(f"UI46 footer glyph audit PASS ({checked} shared helper calls plus standalone labels, Spanish and English)")
