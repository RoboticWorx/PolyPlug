
idf_component_register(

    SRCS
        "src/gpio_funcs.c"
        "src/gpio_task.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos
    
    PRIV_REQUIRES
    	lora sx126x
)