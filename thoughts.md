# Handling Widescreen stuff. 
- These changes should only be implemented IF the user is in widescreen. Original behaviour should be preserved if the original aspect ratio is used.

## Centre the title card
The title card looks fine, but always goes too far left. Make it so it goes to the horizontal centre. 

## Black bar left and rights on end of acts and bosses

Right now, if you hit the capsule at the end of act 1, big black bars are drawn at the sides. These feel reduendent on act 1s. There's no need to restrict the player to the tight original field. 

## Boss Handling

### First zone boss
Restrict the player to an area the set horizontal width of the screen. Have the boss move all the way across, left and right. Apart from the distance he can travel, the loop should be the same.

### Second zone boss
No adjustment to boss behaviour. The player should be restircted to the horizontal width of the screen until the boss is defeated

### Third zone boss
No changes. Works perfectly


Do these first. 