This repository contains a program that is used to kill 
child processes from applications when they die unexpectedly.

Operating systems do not kill child processes when a parent dies,
so when a crash or otherwise unexpected closure occurs, the child
stays around. This is especially obnoxious during debugging when 
killing processes is notably common.