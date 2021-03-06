package com.huawei.blackhole.network.api.bean;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.huawei.blackhole.network.common.constants.Constants;
import com.huawei.blackhole.network.common.constants.PntlInfo;
import com.huawei.blackhole.network.core.bean.Result;
import org.apache.commons.lang3.StringUtils;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.Serializable;
import java.text.DateFormat;
import java.text.DecimalFormat;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Iterator;
import java.util.List;

@JsonIgnoreProperties(ignoreUnknown = true)
@JsonInclude(JsonInclude.Include.NON_NULL)
public class LossRate implements Serializable{
    private static final long serialVersionUID = 7880158023453028072L;
    private static final Logger LOG = LoggerFactory.getLogger(LossRate.class);
    @JsonProperty("result")
    private static List<LossRateResult> result = new ArrayList<LossRateResult>();

    private static int lossRateThreshold = 0;

    public LossRate(){
        this.result = getResult();
    }
    public static List<LossRateResult> getResult() {
        return result;
    }

    public void setResult(List<LossRateResult> result) {
        this.result = result;
    }

    public static int getLossRateThreshold() {
        return lossRateThreshold;
    }

    public static void setLossRateThreshold(int lossRateThreshold) {
        LossRate.lossRateThreshold = lossRateThreshold;
    }

    @JsonIgnoreProperties(ignoreUnknown = true)
    @JsonInclude(JsonInclude.Include.NON_NULL)
    public static final class LossRateResult implements Serializable{
        private static final long serialVersionUID = 317748549189407292L;

        @JsonProperty("src_ip")
        private String srcIp;
        @JsonProperty("dst_ip")
        private String dstIp;
        @JsonProperty("send_loss_rate")
        private String sendLossRate;
        @JsonProperty("send_pkgs")
        private String sendPkgs;
        @JsonProperty("recv_loss_rate")
        private String recvLossRate;
        @JsonProperty("recv_pkgs")
        private String recvPkgs;
        @JsonProperty("time")
        private String timestamp;

        public String getSrcIp() {
            return srcIp;
        }

        public void setSrcIp(String srcIp) {
            this.srcIp = srcIp;
        }

        public String getDstIp() {
            return dstIp;
        }

        public void setDstIp(String dstIp) {
            this.dstIp = dstIp;
        }

        public String getSendLossRate() {
            return sendLossRate;
        }

        public void setSendLossRate(String sendLossRate) {
            this.sendLossRate = sendLossRate;
        }

        public String getSendPkgs() {
            return sendPkgs;
        }

        public void setSendPkgs(String sendPkgs) {
            this.sendPkgs = sendPkgs;
        }

        public String getRecvLossRate() {
            return recvLossRate;
        }

        public void setRecvLossRate(String recvLossRate) {
            this.recvLossRate = recvLossRate;
        }

        public String getRecvPkgs() {
            return recvPkgs;
        }

        public void setRecvPkgs(String recvPkgs) {
            this.recvPkgs = recvPkgs;
        }

        public String getTimestamp() {
            return timestamp;
        }

        public void setTimestamp(String timestamp) {
            this.timestamp = timestamp;
        }
    }

    /**
     * 将agent推送的丢包信息保存在内存中，用于UI展示
     * @param flow
     */
    public static void saveInfo(LossRateAgent.Flow flow){
        String srcIp = flow.getSip();
        String dstIp = flow.getDip();
        boolean hasData = false;

        if (StringUtils.isEmpty(srcIp) || StringUtils.isEmpty(dstIp)){
            return;
        }
        if (StringUtils.isEmpty(flow.getPacketDrops()) || StringUtils.isEmpty(flow.getPacketSent())){
            return;
        }

        LossRateResult newData = new LossRateResult();
        float rate = Float.parseFloat(flow.getPacketDrops()) / Float.parseFloat(flow.getPacketSent());
        DecimalFormat df2 = new DecimalFormat("###.00");
        String recvPkgs = String.valueOf(Integer.valueOf(flow.getPacketSent()) - Integer.valueOf(flow.getPacketDrops()));

        if (Float.valueOf(rate*100).intValue() < LossRate.getLossRateThreshold()){
            return;
        }
        newData.setSrcIp(srcIp);
        newData.setDstIp(dstIp);
        newData.setSendLossRate(df2.format(rate*100)+"%");
        newData.setSendPkgs(flow.getPacketSent());
        newData.setRecvLossRate("0");///TODO:暂时设为0
        newData.setRecvPkgs(recvPkgs);
        newData.setTimestamp(flow.getTime());

        PntlWarning.saveWarnToWarningList(newData);

        List<LossRateResult> resultList = LossRate.result;
        for (LossRateResult result : resultList){
            if (srcIp.equals(result.getSrcIp()) && dstIp.equals(result.getDstIp())){
                resultList.set(resultList.indexOf(result), newData);//replace old data
                hasData = true;
                break;
            }
        }

        if (!hasData){
            resultList.add(newData);
        }
    }

    public static Result<LossRate> getLossRateInfo(){
        Result<LossRate> r = new Result<>();
        LossRate lossRate = new LossRate();
        List<LossRateResult> resultList = getResult();
        lossRate.setResult(resultList);

        r.setModel(lossRate);
        return r;
    }

    public static void refleshLossRateWarning(){
        List<LossRateResult> resultList = getResult();
        if (resultList == null || resultList.isEmpty()){
            return;
        }

        Iterator<LossRateResult> it = resultList.iterator();
        LossRateResult lossRate = null;
        while (it.hasNext()){
            lossRate = it.next();
            DateFormat df = new SimpleDateFormat(Constants.TIME_FORMAT);
            try{
                Date dt = df.parse(lossRate.getTimestamp());
                Long intervalTime = System.currentTimeMillis()/1000 - dt.getTime()/1000;
                if (intervalTime >= PntlInfo.MONITOR_INTERVAL_TIME_NEWEST){
                    LOG.info("Remove warning:" + lossRate.getSrcIp() +" -> " + lossRate.getDstIp());
                    it.remove();
                }
            } catch (ParseException e) {
                e.printStackTrace();
            }
        }
    }
}
