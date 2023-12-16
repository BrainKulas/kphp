@ok access
<?php

class Base {
  private $field = [];
  private function fun1() { echo "Base::fun1\n";}
  public function test() {
    $this->field[] = 1;
    var_dump($this->field);
    $this->fun1();
  }
}

$x = new Base();
$x->test();
