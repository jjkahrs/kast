# Included by the starter_mmo live scripts: character names are unique per realm, so CHARACTER becomes CHARACTER plus
# 6 letters from the test account's email hash, and each account reuses its own characters.
string(MD5 hash "$ENV{KAST_ONLINE_EMAIL}")
string(SUBSTRING "${hash}" 0 6 hash)
foreach(pair 0a 1b 2c 3d 4e 5f 6g 7h 8i 9j)
  string(SUBSTRING "${pair}" 0 1 digit)
  string(SUBSTRING "${pair}" 1 1 letter)
  string(REPLACE "${digit}" "${letter}" hash "${hash}")
endforeach()
set(CHARACTER "${CHARACTER}${hash}")
