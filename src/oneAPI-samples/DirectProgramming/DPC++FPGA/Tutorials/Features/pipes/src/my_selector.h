//
// Created by louxyu on 2022/11/18.
//

#ifndef PIPES_MY_SELECTOR_H
#define PIPES_MY_SELECTOR_H

#include <CL/sycl/device_selector.hpp>
#include <CL/sycl.hpp>

class my_selector: public sycl::device_selector{
    public:
    int  operator()(const sycl::device &dev) const override {
        std::string device_name=dev.get_info<sycl::info::device::name>();
        std::cout<<"devices:"<<device_name<<std::endl;
        if(device_name.find("SYCL host device")!=std::string::npos){
            std::cout<<"my_device:"<<device_name<<std::endl;
            return 1;
        }
        return -1;
    }
};


#endif //PIPES_MY_SELECTOR_H
