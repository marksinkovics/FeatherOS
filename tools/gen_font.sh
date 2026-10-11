#!/bin/bash

chr() {
    [ "$1" -lt 256 ] || return 1
    printf '%b' "\\0$(printf '%03o' "$1")"
}

rm -f -- ./*.xbm
rm -rf char_*

FROM=33
TO=126
TOTAL=$((TO - FROM + 1))
FONT="Ubuntu-Mono"

STR=""
echo -n "> "
for i in {33..126}; do
    if [ "$i" -eq 37 ]; then
        echo -n "%"
        STR="$STR\%"
        convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"%" xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >>font.xbm
        # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"%" char_$i.bmp
    elif [ "$i" -eq 92 ]; then
        echo -n "\\"
        STR="$STR\\"
        convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"/" -flip xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >>font.xbm
        # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"/" -flip char_$i.bmp
    else
        C="$(chr "$i")"
        echo -n "$C"
        STR="$STR$C"
        convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"$C" xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >>font.xbm
        # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"$C" char_$i.bmp
    fi
done
echo ""

echo "FROM: $FROM"
echo "TO: $TO"
echo "TOTAL: $TOTAL"
echo "> $STR"
