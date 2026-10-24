# Rects (x,y,w,h in a 1600x900 shot) the region checks of cmake/voxelgi_tests.cmake compare. voxelgi_room's Scene view:
# the floor by the red wall and by the green wall, the ceiling by the red wall, the chrome ball's left (facing the red
# wall) and right halves. Its Game view in Play: the rough ball's underside (no direct light at either lamp position),
# the chrome ball. lightmap_room's Scene view (cmake/lightmap_regions.cmake): its floor by the red wall, its red wall,
# the Dynamic ball.
set(vgi_red_floor 577,511,30,12)
set(vgi_green_floor 990,505,25,10)
set(vgi_red_ceiling 600,180,40,10)
set(vgi_chrome_left 827,380,28,30)
set(vgi_chrome_right 870,380,28,30)
set(vgi_play_unlit 660,415,25,10)
set(vgi_play_chrome 832,339,20,10)
set(vgi_lm_floor 622,416,30,12)
set(vgi_lm_wall 500,300,40,60)
set(vgi_lm_ball 640,358,20,28)
