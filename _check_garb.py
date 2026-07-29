lines = open(r'main\boards\cuckoo-clock\cuckoo_controller.cc', 'r', encoding='utf-8').read().splitlines()
comment_garb = 0
inline_garb = 0
code_garb = 0

with open('_garb_report.txt', 'w', encoding='utf-8') as f:
    for i, l in enumerate(lines):
        if '\ufffd' not in l:
            continue
        stripped = l.lstrip()
        is_comment = stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*') or stripped.startswith('/**')
        if is_comment:
            comment_garb += 1
            f.write(f'[COMMENT] L{i+1}: {l[:120]}\n')
        elif '//' in l and not stripped.startswith('#'):
            inline_garb += 1
            f.write(f'[INLINE]  L{i+1}: {l[:120]}\n')
        else:
            code_garb += 1
            f.write(f'[CODE]    L{i+1}: {l[:120]}\n')

    f.write(f'\nTotal remaining: {comment_garb} pure comment, {inline_garb} inline, {code_garb} code\n')

print(f'Remaining garbled: {comment_garb} comment + {inline_garb} inline + {code_garb} code = {comment_garb+inline_garb+code_garb}')
print('Details in _garb_report.txt')
