@ok
<?php

  $x[1][] = 4;
	$x[0][1][2] = 5;
	$x[0][1][2][] = 6;
	$x[0][1][2][3] = 7;
	$x[0][1][] = 8;

	var_dump ($x);
?>
