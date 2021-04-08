<?php

$obj = array (
         'store' =>
         array (
           'book' =>
           array (
               'category' => 'reference',
               'title' => 'Sayings of the Century',
               'price' => 8.9499999999999993,
               'author' => 'Nigel Rees',
           )
         )
       );

$jsonPath = new JsonPath();

print_r($jsonPath->find($obj, "$..book"));