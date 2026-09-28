#!/bin/bash

chr() {
  [ "$i" -lt 256 ] || return 1
  if [ "$1" -eq 37 ]; then
    printf "\\%%"
    elif [ "$1" -eq 92 ]; then
    printf "\\\\"
  else
    printf "\\$(printf '%03o' "$1")"
  fi
}

rm *.xbm
rm -rf char_*

FROM=33
TO=126
TOTAL=$(($TO - $FROM + 1))
FONT="Ubuntu-Mono"

STR=""
echo -n "> "
for i in {33..126}
do
  if [ "$i" -eq 37 ]; then
      echo -n "%"
      STR="$STR\%"
      convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"%" xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >> font.xbm
      # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"%" char_$i.bmp
  elif [ "$i" -eq 92 ]; then
      echo -n "\\"
      STR="$STR\\"
      convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"/" -flip xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >> font.xbm
      # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"/" -flip char_$i.bmp
  else
      C="$(chr $i)"
      echo -n "$C"
      STR="$STR$C"
      convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"$C" xbm:- | tail -2 | tr -d '\n' | sed 's/   / /g' | sed 's/  //g' | sed 's/, };/,\n/g' >> font.xbm
      # convert -size 8x16\! -scale 8x16\! -antialias -pointsize 15 -gravity Center -font "$FONT" label:"$C" char_$i.bmp
  fi
done
echo ""

CWIDTH=8
CHEIGHT=16
WIDTH=$(($CWIDTH * $TOTAL))

echo "FROM: $FROM"
echo "TO: $TO"
echo "TOTAL: $TOTAL"
echo "> $STR"