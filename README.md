# Сборщик мусора для языка STELLA

## Summary 

Сборщик мусора основан на идее инкрементальной копирующей сборки (алгоритме Бейкера), для обеспечения лучшей локальности использует алгоритм semi-dfs для обхода структур при копировании. 

## Сборка runtime-а STELLA

Необходимо скомпилировать stella-код в C, после чего 
собрать полученный C-исходник вместе с файлами runtime-а. 

```bash
docker run -i fizruk/stella compile < examples/example.stella  > example.c

gcc -g -DMAX_ALLOC_SIZE=64 -DSTELLA_GC_STATS -std=c11 add2.c stella/runtime.c stella/gc.c -o main
```

При сборке можно указать параметры:

* `-DMAX_ALLOC_SIZE=*размер в байтах*` - размер to-space (и from-space) соответственно 

* `-DSTELLA_GC_DEBUG` - включает логгирование некоторых операций в GC и регулярный dump состояния памяти в кучу

**ВАЖНО: MAX_ALLOC_SIZE стоит выбирать таким образом, чтобы он в каждый момент времени вместил в себя и все объекты, живые в момент инициации сборки мусора и все объекты, созданные до следующей инициации. Это число может быть больше, чем "максимальное количество одновременно живых объектов", т.к. живость в такой схеме определяется не достижимостью из корней в текущий момент времени, а достижимостью из корней в момент последней инициации сборки мусора. Это ограничение является последствием инкрементальности алгоритма, его можно частично обойти ценой значительного его усложнения**

## Формат вывода
### print_gc_stats
Функция `print_gc_stats()` выводит таблицу с различными метриками работы алгоритма:

* Суммарный объём аллокаций
* Максимальная населённость памяти (где живость определяется в том же смысле, что в комментарии выше)
_в силу инкрементальности подсчёт этой статистики может быть не точным, если последний цикл работы gc не завершился. Для надёжности рекомендуется вызвать `gc_force_copy_all`, чтобы гарантированно завершить итерацию_
* Число операций с памятью со стороны программы и runtime-обёрток
* Максимальное число корней за время работы программы 

```
Garbage collector (GC) statistics:
Total memory allocation: 64 bytes (4 objects)
Maximum residency:       64 bytes (4 objects)
Total memory use:        4 reads and 0 writes
Max GC roots stack size: 4 roots
```
### print_gc_state 
Функция `print_gc_stats()` выводит состояние памяти программы и сборщика мусора в текущий момент времени, включая:

* Таблицу значений внутренних переменных GC и их взаимных отношений

```
TO-SPACE: 0x55db4b80d180
FROM-SPACE: 0x55db4b80d120
NEXT pointer: 0x55db4b80d180 (TO-SPACE + 0)
SCAN pointer: 0x55db4b80d180
LIMIT pointer: 0x55db4b80d1a0 (NEXT + 32)
```

* Список объектов, на которые указывают корни программы:

`ROOTS: 0x55db4b80d1a0 0x55db4b80d1b0 0x55db4b80d1a0 0x55db4b80d1b0` 

* Адреса и значения глобальных объектов расположенных не на куче (различаются от программы к программе исключительно адресами)

```
Objects, not handled by GC:
0x55db4b80d110: STELLA OBJECT of 0 fields WITH tag Zero of value 0
0x55db4b80d030: STELLA OBJECT of 0 fields WITH tag Zero of value unit
0x55db4b80d038: STELLA OBJECT of 0 fields WITH tag Zero of value []
0x55db4b80d040: STELLA OBJECT of 0 fields WITH tag Zero of value {}
0x55db4b80d048: STELLA OBJECT of 0 fields WITH tag Zero of value false
0x55db4b80d050: STELLA OBJECT of 0 fields WITH tag Zero of value true
```

* Dump to-space, в котором размечены header-ы объектов (их tag, и текстовое представление, полученное через `print_stella_object`), а также значения полей и их расположение в памяти (from-space, to-space или имя глобального объекта). Для полей из from-space также приложена их dump, строчки с ним начинаются с `|        `

```
to-space:
0x55db4b80d180: NOTHING
0x55db4b80d188: NOTHING
0x55db4b80d190: NOTHING
0x55db4b80d198: NOTHING
0x55db4b80d1a0: STELLA OBJECT of 1 fields WITH tag Succ of value 3
0x55db4b80d1a8: field #0 = 0x55db4b80d1b0(to-space)
0x55db4b80d1b0: STELLA OBJECT of 1 fields WITH tag Succ of value 2
0x55db4b80d1b8: field #0 = 0x55db4b80d1c0(to-space)
0x55db4b80d1c0: STELLA OBJECT of 1 fields WITH tag Succ of value 1
0x55db4b80d1c8: field #0 = 0x55db4b80d110(global ZERO)
```

## Тесты

add2 создаёт n+1 16-байтовый объект, который живёт вечно 

for_flip генерирует 80-байт мусора за итерацию, затем забывает о них