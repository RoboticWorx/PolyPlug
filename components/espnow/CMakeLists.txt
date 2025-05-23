
idf_component_register(

    SRCS
        "src/espnow_funcs.c"
        "src/espnow_task.c"
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos
    
    PRIV_REQUIRES
    	 esp_wifi esp_netif gpio
)