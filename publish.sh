set -e
cd /e/PythonFiles/Bare
git init -b main
git add -A
git -c user.name=ulyees -c user.email=511700825@qq.com commit -m "Bare: 极简 Windows 文本编辑器（单文件 Win32/C++17，含查找与快捷键）"
git remote remove origin 2>/dev/null || true
git remote add origin https://github.com/ulyees/Bare.git
git push -u origin main
git log --oneline -1
git ls-files | head -30
