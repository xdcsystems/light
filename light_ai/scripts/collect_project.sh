#bash
for f in CMakeLists.txt include/*.h include/*.hpp; do   echo "===== $f ====="; cat "$f"; echo; done
for f in src/*.cpp scripts/* README.md; do   echo "===== $f ====="; cat "$f"; echo; done

