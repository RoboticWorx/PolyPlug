# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c         # list the source files of this component

    INCLUDE_DIRS "."       # optional, add here public include directories

    PRIV_INCLUDE_DIRS   # optional, add here private include directories
    
    REQUIRES            # optional, list the public requirements (component names)
    	common
    	
    PRIV_REQUIRES       # optional, list the private requirements
    	lora sx126x gpio esp_netif esp_wifi espnow  wifi
    	
)
