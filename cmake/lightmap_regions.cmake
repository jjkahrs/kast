# Rects (x,y,w,h in a 1600x900 lightmap_room Scene view shot; "a:b" takes the denominator over b) the region checks of
# cmake/lightmap_tests.cmake compare: the floor by the red wall and by the green wall, the open floor over the floor at
# the static Ball's foot, the Dynamic ball by the red wall and at the back.
set(lm_red_floor 622,416,30,12)
set(lm_green_floor 982,446,30,12)
set(lm_contact 882,436,40,15:844,418,24,5)
set(lm_ball_front 640,358,20,28)
set(lm_ball_back 808,338,20,20)
