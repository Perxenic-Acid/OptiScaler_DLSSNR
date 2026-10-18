"""检查翻译缺项；API 名称、格式化数值、内部窗口名保留上游写法。"""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
string = r'"(?:[^"\\]|\\.)*"'
catalog = (root / 'OptiScaler/menu/translations_zh_cn.inc').read_text(encoding='utf-8')
entries = re.findall(r'\{\s*(' + string + r')\s*,\s*(' + string + r')\s*\}', catalog)
keys = {json.loads(key) for key, _ in entries}
assert len(keys) == len(entries), '翻译键重复'
technical = {
    '%d', '%s', '(%d)', '%dx%d -> %dx%d (%.1f) [%dx%d (%.1f)]',
    'D3D11 %s| %s %d.%d.%d%s', 'D3D12 %s| %s %d.%d.%d', 'Vulkan %s| %s %d.%d.%d%s',
    'FGId: %llu, RfxId: %llu', 'DE Ver: %d.%d.%d.%d   GB Ver: %d.%d',
    'FSR 3.1 FG: %s', 'FSR 3.1 SR: %s', 'FSR 3.1: %s', 'FSR Hooks: %s',
    'libxess: %s', 'nvngx.dll: %s', 'nvngx_dlss : %s', 'nvngx_dlssd : %s',
    'DLAA', 'FFX FG', 'HDR', 'MFG', 'RCAS', 'fakenvapi', 'x',
    'Splash', 'DlssNrExposureScan',
}
calls = r'(?:OptiMenu::\w+|ScopedCollapsingHeader|DeferredSlider|PopulateCombo|AddResourceBarrier|' \
        r'AddDLSSRenderPreset|AddDLSSDRenderPreset|ShowTooltip|ShowHelpMarker|HelpMarker)'
pattern = re.compile(calls + r'\(\s*(' + string + r'(?:\s*' + string + r')*)')
missing = set()
count = 0
for name in ['OptiScaler/menu/menu_common.cpp', 'OptiScaler/dlssnr/DlssNr_Menu.cpp']:
    source = (root / name).read_text(encoding='utf-8-sig')
    for match in pattern.finditer(source):
        text = ''.join(json.loads(part) for part in re.findall(string, match[1]))
        visible = text.split('##', 1)[0]
        if not visible or not re.search('[a-zA-Z]', visible):
            continue
        count += 1
        if text not in keys and visible not in keys and text not in technical:
            missing.add(text)
assert not missing, '界面缺少翻译：\n' + '\n'.join(sorted(missing))
print(f'已检查 {len(entries)} 个词条和 {count} 个界面显示调用')
